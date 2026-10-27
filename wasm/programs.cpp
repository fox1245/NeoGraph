// The Program API of the JavaScript module (#556, #557): compile a Program from its source, admit it, start a run, observe
// its events, resume an interrupt, fork a paused run, read a result, cancel. wasm/neograph.mjs is its JavaScript side; this
// file is the C API it calls.
//
//   ng_programs_create(config, stores)            -> handle (0 on failure; ng_error() says why)
//   ng_programs_admit(handle, call, source)               compile and admit a Program
//   ng_programs_start(handle, call, options)              start a run
//   ng_programs_resume(handle, call, options)             resume an interrupted run
//   ng_programs_fork(handle, call, options)               fork an interrupted run at its checkpoint
//   ng_programs_handoff(handle, call, options)            wait for and read a held checkpoint handoff
//   ng_programs_release_handoff(handle, call, run)        release a held checkpoint handoff so the run continues
//   ng_programs_replace(handle, call, options)            replace a handed-off run with another admitted version
//   ng_programs_migrate_graph(handle, call, options)      migrate a live root Core graph run at a safe point
//   ng_programs_synthesize_child(handle, call, options)   compile, admit and publish a proposed child Program
//   ng_programs_cancel(handle, call, run)                 cancel a run
//   ng_programs_result(handle, call, run)                 read what a run left in the transition store
//   ng_programs_release(handle)
//
// Every call but create and release returns at once. Its outcome arrives through Module.ngProgramDone(call, text), a JSON
// object `{ok: true, result}` or `{ok: false, error}`, from a later turn of the host's event loop; the events of a run
// started with `stream` arrive through Module.ngProgramEvent(call, text) before it. Nothing here waits: the runtime runs on
// the loop of this module instance (async::HostLoop), awaits its stores there, and the SDK sends over fetch on the same
// executor. A store the host supplies may complete a call in a later turn of that loop.
#include "module_host.h"

#include <neograph/async/host_loop.h>
#include <neograph/graph/cancel.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/program/child_synthesis.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/program.h>
#include <neograph/program/replacement.h>
#include <neograph/program/store_call.h>
#include <neograph/program/synthesis.h>

#include <asio/async_result.hpp>
#include <asio/bind_executor.hpp>
#include <asio/post.hpp>
#include <asio/redirect_error.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <emscripten.h>

#ifdef NEOGRAPH_JS_STORES
#include "js_stores.h"
#endif

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "operation_lock.h"
#include "provider_binding.h"

namespace {

using neograph::json;
namespace graph = neograph::graph;
using namespace neograph::program;
using neograph::wasm::loop;

EM_JS(void, ng_js_program_event, (int call, const char* text), { Module.ngProgramEvent(call, UTF8ToString(text)); });
EM_JS(void, ng_js_program_done, (int call, const char* text), { Module.ngProgramDone(call, UTF8ToString(text)); });
// The registration of the host's stores object ends when the last adapter that was made for it is gone.
EM_JS(void, ng_js_stores_free, (int handle), { if (Module.ngProgramsStoresFree) Module.ngProgramsStoresFree(handle); });
EM_JS_DEPS(ng_program_deps, "$UTF8ToString");

std::string digest(char c) { return "sha256:" + std::string(64, c); }

ExecutableManifest manifest(ExecutableIdentity identity, std::vector<ExecutableIdentity> requires_ = {}) {
    return ExecutableManifest{std::move(identity), EffectMode::Brokered, "attestation:js-module", {}, {},
                              std::move(requires_)};
}

// What a Program of this module can name: the model call and the two reducers a channel uses. The provider is the one the
// host described; its key and model are not part of any identity.
const ExecutableIdentity kProviderIdentity{ExecutableKind::Provider, "js-host-provider", "1.0.0", digest('1')};

RegistrySnapshot make_registry() {
    RegistrySnapshotBuilder builder;
    builder.add_provider(manifest(kProviderIdentity), ProviderMetadata{json::object(), json::object()});
    builder.add_core_llm_call(manifest({ExecutableKind::Node, "llm_call", "1.0.0", digest('2')}, {kProviderIdentity}),
                              {});
    builder.add_core_tool_dispatch(manifest({ExecutableKind::Node, "tool_dispatch", "1.0.0", digest('5')}), {});
    builder.add_reducer(manifest({ExecutableKind::Reducer, "overwrite", "1.0.0", digest('3')}),
                        [](const json&, const json& incoming) { return incoming; });
    builder.add_reducer(manifest({ExecutableKind::Reducer, "append", "1.0.0", digest('4')}),
                        [](const json& current, const json& incoming) {
                            json merged = current.is_array() ? current : json::array();
                            if (incoming.is_array())
                                for (const auto& item : incoming) merged.push_back(item);
                            else
                                merged.push_back(incoming);
                            return merged;
                        });
    return std::move(builder).build();
}

AdmissionProfile make_profile(const RegistrySnapshot& registry,
                              std::uint32_t max_supported_child_depth) {
    AdmissionProfileBuilder builder;
    builder.id("js-module-profile")
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

PolicySnapshot make_policy(const AdmissionProfile& profile,
                           const std::string& owner,
                           const BudgetLimits& ceiling) {
    PolicySnapshotBuilder builder;
    builder.id("js-module-policy")
        .semantic_version("1.0.0")
        .owner_scope(owner)
        .admission_profile(profile)
        .budget_ceiling(ceiling)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    return std::move(builder).build();
}

std::uint32_t child_depth_ceiling_from(const json& config) {
    if (!config.is_object() || !config.contains("maxSupportedChildDepth")) {
        return MAX_SUPPORTED_CHILD_DEPTH;
    }
    const auto& value = config.at("maxSupportedChildDepth");
    if (!value.is_number()) {
        throw std::invalid_argument("maxSupportedChildDepth must be a whole number from 1 to " +
                                    std::to_string(MAX_REPRESENTABLE_CHILD_DEPTH));
    }
    const double amount = value.get<double>();
    if (!(amount >= 1.0) || amount > static_cast<double>(MAX_REPRESENTABLE_CHILD_DEPTH) ||
        amount != std::floor(amount)) {
        throw std::invalid_argument("maxSupportedChildDepth must be a whole number from 1 to " +
                                    std::to_string(MAX_REPRESENTABLE_CHILD_DEPTH));
    }
    return validate_max_supported_child_depth(static_cast<std::uint64_t>(amount));
}

// A budget written the way JavaScript writes it: `{wallTimeMs, modelTokens, ...}`; a member that is absent keeps `base`.
// An amount that is not a whole number from 0 to 2^53, and a name that is not a member, is refused: a misspelt limit must
// not leave the default in force unnoticed.
RunBudget budget_from(const json& description, RunBudget base) {
    if (description.is_null()) return base;
    if (!description.is_object()) throw std::invalid_argument("a budget is an object of amounts");
    constexpr double largest = 9007199254740992.0;  // 2^53, the largest whole number a JavaScript number holds
    const auto       number  = [&](const std::string& name, auto& target) {
        const auto& value = description.at(name);
        if (!value.is_number()) throw std::invalid_argument("budget." + name + " must be a number");
        using Target        = std::decay_t<decltype(target)>;
        const double amount = value.get<double>();
        const double top    = std::min(largest, static_cast<double>(std::numeric_limits<Target>::max()));
        if (!(amount >= 0.0) || amount > top || amount != std::floor(amount))
            throw std::invalid_argument("budget." + name + " must be a whole number from 0 to " +
                                        (top == largest ? std::string("2^53") : std::to_string(static_cast<std::uint64_t>(top))));
        target = static_cast<Target>(amount);
    };
    for (const auto& [name, value] : description.items()) {
        (void)value;
        if (name == "wallTimeMs") number(name, base.wall_time_ms);
        else if (name == "modelTokens") number(name, base.model_tokens);
        else if (name == "monetaryMicrounits") number(name, base.monetary_microunits);
        else if (name == "maxConcurrency") number(name, base.max_concurrency);
        else if (name == "maxProgramOperations") number(name, base.max_program_operations);
        else if (name == "maxCoreSteps") number(name, base.max_core_steps);
        else if (name == "maxDynamicCompiles") number(name, base.max_dynamic_compiles);
        else if (name == "maxChildDepth") number(name, base.max_child_depth);
        else if (name == "maxTotalChildren") number(name, base.max_total_children);
        else throw std::invalid_argument("budget has no member " + name);
    }
    return base;
}

// What a developer can set of the QuickJS control runtime, written the way JavaScript writes it: `{stackBytes, defineTimeMs}`.
// A member that is absent keeps its default. Like a budget, an amount that is not a whole number within its bounds, and a
// name that is not a member, is refused, and the message states the bounds.
struct JavaScriptOptions {
    std::size_t   stack_bytes    = JavaScriptCompileLimits::kDefaultStackBytes;
    std::uint64_t define_time_ms = 5'000;
};

// The stack counts the module's own stack in linear memory (deps/quickjs/NEOGRAPH_PROVENANCE). Its floor is what a small
// Program needs to be admitted and run (wasm/test/program_test.mjs runs one at the floor), its ceiling what that stack
// holds. What the host's call stack carries is below the ceiling, and is the developer's to judge (wasm/README.md).
constexpr std::size_t   kMinStackBytes        = 16 * 1024;
constexpr std::uint64_t kMaxDefineTimeMs      = 3'600'000;

JavaScriptOptions javascript_options_from(const json& description) {
    JavaScriptOptions options;
    if (description.is_null()) return options;
    if (!description.is_object()) throw std::invalid_argument("javascript is an object of amounts");
    const auto range = [](std::uint64_t smallest, std::uint64_t largest) {
        return "a whole number from " + std::to_string(smallest) + " to " + std::to_string(largest);
    };
    const auto whole = [&](const std::string& name, std::uint64_t smallest, std::uint64_t largest) -> std::uint64_t {
        const auto& value = description.at(name);
        // Every refusal of a member states its bounds, whatever was wrong with the value.
        if (!value.is_number()) throw std::invalid_argument("javascript." + name + " must be " + range(smallest, largest));
        const double amount = value.get<double>();
        if (!(amount >= static_cast<double>(smallest)) || amount > static_cast<double>(largest) || amount != std::floor(amount))
            throw std::invalid_argument("javascript." + name + " must be " + range(smallest, largest));
        return static_cast<std::uint64_t>(amount);
    };
    for (const auto& [name, value] : description.items()) {
        (void)value;
        if (name == "stackBytes") options.stack_bytes = static_cast<std::size_t>(whole(name, kMinStackBytes, javascript_stack_ceiling()));
        else if (name == "defineTimeMs") options.define_time_ms = whole(name, 1, kMaxDefineTimeMs);
        else
            throw std::invalid_argument("javascript has no member " + name + ": stackBytes is " +
                                        range(kMinStackBytes, javascript_stack_ceiling()) + ", defineTimeMs is " +
                                        range(1, kMaxDefineTimeMs));
    }
    return options;
}

json budget_json(const RunBudget& budget) {
    return json{{"wallTimeMs", budget.wall_time_ms},
                {"modelTokens", budget.model_tokens},
                {"monetaryMicrounits", budget.monetary_microunits},
                {"maxConcurrency", budget.max_concurrency},
                {"maxProgramOperations", budget.max_program_operations},
                {"maxCoreSteps", budget.max_core_steps},
                {"maxDynamicCompiles", budget.max_dynamic_compiles},
                {"maxChildDepth", budget.max_child_depth},
                {"maxTotalChildren", budget.max_total_children}};
}

BudgetLimits limits_of(const RunBudget& budget) {
    return BudgetLimits{budget.wall_time_ms,
                        budget.model_tokens,
                        budget.monetary_microunits,
                        budget.max_concurrency,
                        budget.max_program_operations,
                        budget.max_core_steps,
                        static_cast<std::uint32_t>(budget.max_dynamic_compiles),
                        budget.max_child_depth,
                        budget.max_total_children};
}

// A fork takes half of every amount the paused run has left; a limit (concurrency) is the source's own. The fork carries
// no child budget.
RunBudget fork_share(RunBudget remaining) {
    const auto half = [](auto value) { return value > 1 ? value / 2 : value; };
    remaining.wall_time_ms           = half(remaining.wall_time_ms);
    remaining.model_tokens           = half(remaining.model_tokens);
    remaining.monetary_microunits    = half(remaining.monetary_microunits);
    remaining.max_program_operations = half(remaining.max_program_operations);
    remaining.max_core_steps         = half(remaining.max_core_steps);
    remaining.max_dynamic_compiles   = half(remaining.max_dynamic_compiles);
    remaining.max_child_depth        = 0;
    remaining.max_total_children     = 0;
    remaining.max_node_retries.reset();
    return remaining;
}

json result_json(const ProgramResult& result) {
    json out = json::object();
    out["runId"]     = result.run_id();
    out["versionId"] = result.program_version_id();
    out["status"]    = std::string(to_string(result.status()));
    out["output"]    = result.output();
    out["trace"]     = result.execution_trace();
    const auto usage = result.usage();
    out["usage"]     = json{{"wallTimeMs", usage.wall_time_ms},
                            {"modelTokens", usage.model_tokens},
                            {"programOperations", usage.program_operations},
                            {"coreSteps", usage.core_steps}};
    out["remainingBudget"] = budget_json(result.remaining_budget());
    if (const auto checkpoint = result.checkpoint()) out["checkpointId"] = checkpoint->checkpoint_id;
    if (const auto interrupt = result.interrupt()) {
        json detail = json::object();
        detail["node"]  = interrupt->core_node;
        detail["value"] = interrupt->value;
        if (interrupt->pending_input) {
            detail["kind"]      = "input";
            detail["pendingId"] = interrupt->pending_input->call_id();
        } else if (interrupt->pending_effect) {
            detail["kind"]      = "effect";
            detail["pendingId"] = interrupt->pending_effect->call_id();
        }
        out["interrupt"] = std::move(detail);
    }
    if (const auto failure = result.failure()) {
        out["failure"] = json{{"code", failure->code},
                              {"message", failure->message},
                              {"node", failure->core_node},
                              {"attempts", failure->attempts},
                              {"witness", failure->witness}};
    }
    return out;
}

// What a failed call says. The typed detail is the cause's: a Program diagnostic carries its code, a compile or admission
// refusal its list of diagnostics.
json diagnostic_json(const Diagnostic& diagnostic) {
    return json{{"code", diagnostic.code},
                {"message", diagnostic.message},
                {"pointer", diagnostic.primary.json_pointer},
                {"witness", diagnostic.witness}};
}

json failure_json(const std::exception_ptr& error) {
    json detail = json::object();
    try {
        std::rethrow_exception(error);
    } catch (const ProgramCompileError& failure) {
        detail["type"]    = "compile";
        detail["message"] = std::string(failure.what());
        json diagnostics  = json::array();
        for (const auto& diagnostic : failure.diagnostics()) diagnostics.push_back(diagnostic_json(diagnostic));
        detail["diagnostics"] = std::move(diagnostics);
    } catch (const ProgramAdmissionError& failure) {
        detail["type"]    = "admission";
        detail["message"] = std::string(failure.what());
        json diagnostics  = json::array();
        for (const auto& diagnostic : failure.diagnostics()) diagnostics.push_back(diagnostic_json(diagnostic));
        detail["diagnostics"] = std::move(diagnostics);
    } catch (const ProgramDiagnosticError& failure) {
        detail["type"]    = "program";
        detail["message"] = std::string(failure.what());
        detail["code"]    = failure.diagnostic().code;
        detail["witness"] = failure.diagnostic().witness;
    } catch (const ProgramChildSynthesisError& failure) {
        detail["type"]    = "program";
        detail["message"] = std::string(failure.what());
        detail["code"]    = failure.code();
        detail["witness"] = json{{"code", failure.code()}};
    } catch (const ProgramBlockingCallRefused& failure) {
        detail["type"]    = "refused";
        detail["message"] = std::string(failure.what());
    } catch (const graph::CancelledException& failure) {
        detail["type"]    = "cancelled";
        detail["message"] = std::string(failure.what());
    } catch (const std::exception& failure) {
        detail["type"]    = "error";
        detail["message"] = std::string(failure.what());
    } catch (...) {
        detail["type"]    = "error";
        detail["message"] = "unknown failure";
    }
    return json{{"ok", false}, {"error", std::move(detail)}};
}

// The events of one run, reported to JavaScript as they happen. An exception from the handler cannot cross this call, so
// the JavaScript side catches its own.
class JsEventSink final : public ProgramEventSink {
public:
    explicit JsEventSink(int call) : call_(call) {}
    void on_event(const ProgramEvent& event) override { ng_js_program_event(call_, event.serialize_canonical().c_str()); }

private:
    int call_;
};

std::string_view child_synthesis_state_name(ProgramChildSynthesisState state) {
    switch (state) {
        case ProgramChildSynthesisState::Reserved: return "reserved";
        case ProgramChildSynthesisState::Compiling: return "compiling";
        case ProgramChildSynthesisState::Compiled: return "compiled";
        case ProgramChildSynthesisState::Validating: return "validating";
        case ProgramChildSynthesisState::Validated: return "validated";
        case ProgramChildSynthesisState::Admitting: return "admitting";
        case ProgramChildSynthesisState::Admitted: return "admitted";
        case ProgramChildSynthesisState::Bound: return "bound";
        case ProgramChildSynthesisState::Dispatching: return "dispatching";
        case ProgramChildSynthesisState::Spawned: return "spawned";
        case ProgramChildSynthesisState::Failed: return "failed";
        case ProgramChildSynthesisState::ReconciliationRequired: return "reconciliation_required";
    }
    return "";
}

// One Programs object of JavaScript: the stores, the catalog that admits Programs into them, and the runtime that runs them.
struct Host {
    std::string owner;
    RegistrySnapshot registry = make_registry();
    AdmissionProfile profile  = make_profile(registry, MAX_SUPPORTED_CHILD_DEPTH);
    std::optional<PolicySnapshot> policy;
    std::shared_ptr<ProgramCompiler> compiler;
    RunBudget default_budget;
    std::shared_ptr<ProgramStore>           programs;
    std::shared_ptr<ProgramTransitionStore> transitions;
    std::shared_ptr<ProgramCatalog>         catalog;
    std::shared_ptr<ProgramSynthesisGateway> synthesis_gateway;
    std::unique_ptr<ProgramRuntime>         runtime;
    // The handles of the runs this object started, by run id, for cancel. A run that reached its end is forgotten.
    std::map<std::string, ProgramHandle> live;
    // A run whose start, resume or fork has not returned its handle yet cannot be cancelled through it. A cancel that
    // arrives meanwhile (an event handler that threw does that) is kept and applied once the handle exists.
    std::set<std::string> starting;
    std::set<std::string> cancel_requested;
    // The forks of one paused run take their turns (see fork_handle), one lock per paused run. A lock lives while a fork holds
    // or awaits it, and its entry is dropped by the next fork that finds it expired.
    std::map<std::string, std::weak_ptr<neograph::program::detail::OperationLock>> fork_turns;
    std::map<std::string, std::weak_ptr<neograph::program::detail::OperationLock>> handoff_turns;
    std::map<std::string, ProgramHandoff> held_handoffs;
    std::map<std::string, std::vector<std::function<void()>>> start_waiters;
    std::map<std::string, std::vector<std::function<void()>>> handoff_waiters;
    std::map<std::string, ProgramChildSynthesisGrant> synthesis_grants;
    std::map<std::string, ProgramSynthesisSemanticDecision> synthesis_decisions;
    ProgramSynthesisAsyncSemanticValidator synthesis_validator;
    // Runs whose held handoff a synthesis is using: releasing, replacing or cancelling them waits for it to finish.
    std::set<std::string> handoffs_in_use;
    bool closed = false;

    static void wake_waiters(std::map<std::string, std::vector<std::function<void()>>>& map,
                             const std::string& run_id) {
        const auto found = map.find(run_id);
        if (found == map.end()) return;
        auto waiters = std::move(found->second);
        map.erase(found);
        for (auto& wake : waiters)
            if (wake) asio::post(loop().executor(), std::move(wake));
    }

    static asio::awaitable<void> wait_on(std::map<std::string, std::vector<std::function<void()>>>& map,
                                         const std::string& run_id) {
        co_await asio::async_initiate<const asio::use_awaitable_t<>, void()>(
            [&map, run_id](auto handler) {
                auto shared = std::make_shared<std::decay_t<decltype(handler)>>(std::move(handler));
                map[run_id].push_back([shared]() mutable { (*shared)(); });
            },
            asio::use_awaitable);
    }

    Host(const json& config, int stores_handle) {
        owner = config.value("owner", "js:programs");
        const auto max_supported_child_depth = child_depth_ceiling_from(config);
        profile = make_profile(registry, max_supported_child_depth);
        const auto default_max_child_depth =
            std::min<std::uint32_t>(4u, max_supported_child_depth);
        const auto ceiling = budget_from(
            config.value("limits", json::object()),
            RunBudget{3'600'000, 100'000'000, 1'000'000'000, 16, 100'000, 100'000, 1,
                      default_max_child_depth, 4});
        default_budget     = budget_from(config.value("budget", json::object()),
                                         RunBudget{60'000, 10'000, 1'000'000, 4, 32, 100, 0, 0, 0});
        const auto javascript = javascript_options_from(config.value("javascript", json()));
        policy.emplace(make_policy(profile, owner, limits_of(ceiling)));

        std::shared_ptr<const ModuleStore>      modules;
        std::shared_ptr<TaskGraphFragmentStore> fragments;
#ifdef NEOGRAPH_JS_STORES
        if (stores_handle != 0) {
            // The registration lasts as long as an adapter does, so a run that is cancelled when the object is released can
            // still publish its end through the stores.
            std::shared_ptr<const void> lease(new int(stores_handle), [](const void* held) {
                ng_js_stores_free(*static_cast<const int*>(held));
                delete static_cast<const int*>(held);
            });
            auto set  = neograph::wasm::adapt_js_stores(stores_handle, std::move(lease));
            programs  = std::move(set.programs);
            modules   = std::move(set.modules);
            fragments = std::move(set.fragments);
            transitions = std::move(set.transitions);
        }
#else
        if (stores_handle != 0) throw std::invalid_argument("this module was built without JavaScript stores");
#endif
        if (!programs) programs = std::make_shared<InMemoryProgramStore>();
        if (!modules) modules = std::make_shared<InMemoryModuleStore>();
        if (!fragments) fragments = std::make_shared<InMemoryTaskGraphFragmentStore>();
        if (!transitions) transitions = std::make_shared<InMemoryProgramTransitionStore>();

        auto context = neograph::wasm::node_context_from(config.at("provider"));
        // A control source runs in QuickJS on the one thread of the host's loop, so a source that never ends holds the loop
        // until its time limit. The default limit of a minute is too long for that thread, so define() gets 5 seconds
        // unless the developer chooses another time. The stack is the developer's choice too (see javascript_options_from).
        JavaScriptCompileLimits javascript_limits;
        javascript_limits.max_wall_time_ms = javascript.define_time_ms;
        javascript_limits.max_stack_bytes  = javascript.stack_bytes;
        ProgramCompilerConfig compiler_config{"js-module/1", javascript_limits};
        compiler_config.max_supported_child_depth = max_supported_child_depth;
        compiler = std::make_shared<ProgramCompiler>(registry, std::move(compiler_config));

        CatalogConfig catalog_config{programs, registry, std::make_shared<EngineGenerationCache>(), "js-module/1"};
        catalog_config.capability_binder =
            neograph::program::detail::single_provider_binder(std::move(context), "js-host");
        catalog_config.module_store      = modules;
        catalog                          = std::make_shared<ProgramCatalog>(std::move(catalog_config));

        ProgramSynthesisGatewayConfig synthesis_config;
        synthesis_config.compiler = compiler;
        synthesis_config.catalog  = catalog;
        // The decision is the host's own (synthesizeChild's `semanticDecision` or the validator registered with
        // `registerSynthesisValidator`); the module decides nothing itself. The gateway accepts it only when it names
        // the validator and contract of the host's grant.
        synthesis_config.validate_semantics_async =
            [this](const ProgramSynthesisProposal&    proposal,
                   const ProgramBundle&               bundle,
                   const ProgramSynthesisReservation& reservation) {
            return validate_synthesis(proposal, bundle, reservation);
        };
        synthesis_config.admission = [this](const ProgramSynthesisProposal&, const ProgramBundle&,
                                            const ProgramSynthesisReservation&) {
            return ProgramAdmission{owner, profile, *policy, {}};
        };
        synthesis_gateway = std::make_shared<ProgramSynthesisGateway>(std::move(synthesis_config));

        RuntimeConfig runtime_config;
        runtime_config.catalog             = catalog;
        runtime_config.checkpoints         = std::make_shared<graph::InMemoryCheckpointStore>();
        runtime_config.transitions         = transitions;
        runtime_config.task_graph_fragments = fragments;
        runtime_config.executor            = loop().executor();
        runtime_config.javascript_stack_bytes = javascript.stack_bytes;
        runtime_config.child_synthesis_gateway = synthesis_gateway;
        runtime_config.child_synthesis_grant_resolver =
            [this](std::string_view scope, std::string_view parent_run, std::string_view grant_id)
                -> std::optional<ProgramChildSynthesisGrant> {
            if (scope != owner) return std::nullopt;
            const auto found = synthesis_grants.find(std::string(grant_id));
            if (found == synthesis_grants.end() || found->second.data().parent_run_id != parent_run) return std::nullopt;
            return found->second;
        };
        runtime_config.checkpoint_handler = [this](ProgramHandle handle, ProgramHandoff lease) {
            if (closed) return;
            const auto run_id = handle.run_id();
            live.insert_or_assign(run_id, handle);
            held_handoffs.insert_or_assign(run_id, std::move(lease));
            wake_waiters(start_waiters, run_id);
            wake_waiters(handoff_waiters, run_id);
            wake_waiters(handoff_waiters, "");
        };
        runtime = std::make_unique<ProgramRuntime>(std::move(runtime_config));
    }

    asio::awaitable<ProgramSynthesisSemanticDecision> validate_synthesis(
        const ProgramSynthesisProposal&    proposal,
        const ProgramBundle&               bundle,
        const ProgramSynthesisReservation& reservation) {
        if (const auto found = synthesis_decisions.find(proposal.id()); found != synthesis_decisions.end()) {
            co_return found->second;
        }
        const auto validator = synthesis_validator;
        if (!validator) {
            throw std::invalid_argument(
                "synthesizeChild() needs the host's semanticDecision or registerSynthesisValidator()");
        }
        co_return co_await validator(proposal, bundle, reservation);
    }

    RunInvocation invocation(const json& options, const std::string& version_id, const std::string& run_id,
                             RunBudget budget, json input) const {
        RunInvocation request;
        request.owner_scope        = owner;
        request.agent_id           = "neograph-js";
        request.program_version_id = version_id;
        request.run_id             = run_id;
        request.budget             = std::move(budget);
        request.input              = std::move(input);
        request.message_sequence   = 1;
        request.idempotency_key    = "js:" + run_id;
        request.correlation_id     = options.value("traceId", "js:" + run_id);
        request.validate();
        return request;
    }

    // The run `run_id` is being started, resumed or forked until `arrived` or `abandoned`. Only one call at a time may be.
    void begin_run(const std::string& run_id) {
        if (!starting.insert(run_id).second)
            throw std::invalid_argument("run " + run_id + " is already being started, resumed or forked");
        cancel_requested.erase(run_id);
    }
    asio::awaitable<void> arrived(const ProgramHandle& handle) {
        const auto run_id = handle.run_id();
        live.insert_or_assign(run_id, handle);
        starting.erase(run_id);
        wake_waiters(start_waiters, run_id);
        if (cancel_requested.erase(run_id) != 0) {
            auto copy = handle;
            (void)co_await copy.cancel_async();
        }
    }
    void abandoned(const std::string& run_id) {
        starting.erase(run_id);
        cancel_requested.erase(run_id);
        wake_waiters(start_waiters, run_id);
        wake_waiters(handoff_waiters, run_id);
    }
    void settle(const ProgramResult& result) {
        // An interrupted run waits for its answer: its handle stays. Any other end forgets it.
        if (result.status() != ProgramTerminalStatus::Interrupted) {
            live.erase(result.run_id());
            held_handoffs.erase(result.run_id());
        }
        wake_waiters(handoff_waiters, result.run_id());
        wake_waiters(handoff_waiters, "");
    }

    static ProgramSource source_from(const json& source, const std::string& fallback_name = "js:program") {
        if (source.is_string()) return ProgramSource::from_javascript(fallback_name, source.get<std::string>());
        if (!source.is_object()) throw std::invalid_argument("a Program source is {javascript: text} or {document: object}");
        const std::string name = source.value("name", fallback_name);
        if (source.contains("javascript"))
            return ProgramSource::from_javascript(name, source.at("javascript").get<std::string>());
        if (source.contains("document")) {
            const auto& document = source.at("document");
            return ProgramSource::from_cpp_builder(
                name, document.value("program_schema_version", PROGRAM_SCHEMA_VERSION_V1), document);
        }
        throw std::invalid_argument("a Program source is {javascript: text} or {document: object}");
    }

    asio::awaitable<json> admit(json source) {
        const std::string name    = source.is_object() ? source.value("name", "js:program") : "js:program";
        const auto        program = source_from(source, name);
        // The host's budget for runs of this version: `budget` is the exact grant, `budgetBounds` ({minimum, maximum})
        // the range a grant may take (a replacement carries the remaining budget, which is below the first grant).
        // Members either leaves out keep the host's default budget (maximum) or nothing (minimum). Without them the
        // source's own declaration applies, as before.
        std::optional<ProgramBundle> bundle;
        const bool javascript_source = source.is_object() && program.kind() == SourceKind::JavaScript;
        if (javascript_source && source.contains("budgetBounds")) {
            const auto& bounds = source.at("budgetBounds");
            if (!bounds.is_object()) throw std::invalid_argument("budgetBounds is {minimum, maximum}");
            bundle.emplace(compiler->compile(
                program, ProgramBudgetBounds{budget_from(bounds.value("minimum", json::object()), RunBudget{}),
                                             budget_from(bounds.value("maximum", json::object()), default_budget)}));
        } else if (javascript_source && source.contains("budget")) {
            bundle.emplace(compiler->compile(program, budget_from(source.at("budget"), default_budget)));
        } else {
            bundle.emplace(compiler->compile(program));
        }
        auto version = co_await catalog->admit_async(*bundle, ProgramAdmission{owner, profile, *policy, {}});
        co_return json{{"id", version.id()}, {"bundleId", version.bundle_id()}, {"name", name}};
    }

    asio::awaitable<json> start(json options, int call) {
        const auto version = options.at("version").get<std::string>();
        const auto run_id  = options.at("runId").get<std::string>();
        auto request = invocation(options, version, run_id, budget_from(options.value("budget", json::object()), default_budget),
                                  options.value("input", json::object()));
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        begin_run(run_id);
        std::optional<ProgramHandle> handle;
        try {
            handle.emplace(co_await runtime->start_async(std::move(request), std::move(sink)));
        } catch (...) {
            abandoned(run_id);
            throw;
        }
        co_await arrived(*handle);
        auto result = co_await handle->wait_async();
        settle(result);
        co_return result_json(result);
    }

    asio::awaitable<json> resume(json options, int call) {
        const auto run_id = options.at("runId").get<std::string>();
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        ProgramResume answer{options.value("value", json()), options.value("traceId", "js:resume:" + run_id),
                             std::move(sink), options.at("pendingId").get<std::string>()};
        begin_run(run_id);
        std::optional<ProgramHandle> handle;
        try {
            handle.emplace(co_await runtime->resume_handle_async(owner, run_id, std::move(answer)));
        } catch (...) {
            abandoned(run_id);
            throw;
        }
        co_await arrived(*handle);
        auto result = co_await handle->wait_async();
        settle(result);
        co_return result_json(result);
    }

    asio::awaitable<json> fork(json options, int call) {
        const auto source_run = options.at("runId").get<std::string>();
        const auto new_run    = options.at("newRunId").get<std::string>();
        const auto version    = options.at("version").get<std::string>();
        // From here until the handle exists the fork can be cancelled by name, whatever its turn and the stores take to answer.
        begin_run(new_run);
        std::optional<ProgramHandle> handle;
        try {
            handle.emplace(co_await fork_handle(std::move(options), call, source_run, new_run, version));
        } catch (...) {
            abandoned(new_run);
            throw;
        }
        co_await arrived(*handle);
        auto result = co_await handle->wait_async();
        settle(result);
        co_return result_json(result);
    }

    std::shared_ptr<neograph::program::detail::OperationLock> fork_turns_of(const std::string& source_run) {
        auto& slot = fork_turns[source_run];
        if (auto held = slot.lock()) return held;
        auto made = std::make_shared<neograph::program::detail::OperationLock>();
        slot      = made;
        std::erase_if(fork_turns, [](const auto& entry) { return entry.second.expired(); });
        return made;
    }

    std::shared_ptr<neograph::program::detail::OperationLock> handoff_turns_of(const std::string& run_id) {
        auto& slot = handoff_turns[run_id];
        if (auto held = slot.lock()) return held;
        auto made = std::make_shared<neograph::program::detail::OperationLock>();
        slot      = made;
        std::erase_if(handoff_turns, [](const auto& entry) { return entry.second.expired(); });
        return made;
    }

    asio::awaitable<ProgramHandle> wait_for_handle(const std::string& run_id) {
        while (starting.contains(run_id) && !live.contains(run_id)) {
            co_await wait_on(start_waiters, run_id);
        }
        if (const auto found = live.find(run_id); found != live.end()) co_return found->second;
        auto reconnected = co_await runtime->reconnect_async(owner, run_id);
        live.insert_or_assign(run_id, reconnected);
        co_return reconnected;
    }

    asio::awaitable<ProgramHandoff*> acquire_handoff(const std::string& run_id) {
        const auto turns = handoff_turns_of(run_id);
        const auto turn  = co_await turns->lock_async();
        if (run_id.empty()) {
            while (!closed) {
                for (auto& [_, lease] : held_handoffs)
                    if (lease) {
                        (void)co_await lease.wait_async();
                        co_return &lease;
                    }
                if (live.empty() && starting.empty()) break;
                co_await wait_on(handoff_waiters, "");
            }
            throw std::invalid_argument("no active run with a held checkpoint handoff");
        }
        if (const auto found = held_handoffs.find(run_id); found != held_handoffs.end() && found->second) {
            (void)co_await found->second.wait_async();
            co_return &found->second;
        }
        auto handle = co_await wait_for_handle(run_id);
        while (!closed) {
            if (const auto found = held_handoffs.find(run_id); found != held_handoffs.end() && found->second) {
                (void)co_await found->second.wait_async();
                co_return &found->second;
            }
            if (handle.try_result().has_value()) break;
            if (const auto record = co_await transitions->load(owner, run_id);
                record && record->terminal_result().has_value()) {
                break;
            }
            co_await wait_on(handoff_waiters, run_id);
        }
        throw std::invalid_argument("no active run with a held checkpoint handoff");
    }

    static json handoff_json(const ExactProgramHandoffReference& ref, const json& value) {
        return json{{"runId", ref.source_run_id},
                    {"journalHead", ref.source_journal_head},
                    {"coordinateId", ref.command_coordinate_id},
                    {"entryId", ref.command_entry_id},
                    {"value", value}};
    }

    asio::awaitable<json> handoff(json options) {
        const auto run_id = options.is_string()
                                ? options.get<std::string>()
                                : (options.is_object() && options.contains("runId") && options.at("runId").is_string()
                                       ? options.at("runId").get<std::string>()
                                       : std::string{});
        const auto* lease = co_await acquire_handoff(run_id);
        co_return handoff_json(lease->reference(), lease->value());
    }

    asio::awaitable<json> release_handoff(std::string run_id) {
        if (handoffs_in_use.contains(run_id))
            throw std::invalid_argument("run " + run_id + " has a synthesis in progress on its handoff");
        const auto found = held_handoffs.find(run_id);
        if (found == held_handoffs.end() || !found->second) co_return json(false);
        held_handoffs.erase(found);
        co_return json(true);
    }

    asio::awaitable<ExactProgramHandoff> resolve_handoff_for_replace(const std::string& source_run) {
        if (held_handoffs.contains(source_run) || live.contains(source_run) || starting.contains(source_run)) {
            const auto* lease = co_await acquire_handoff(source_run);
            co_return ExactProgramHandoff{lease->reference(), lease->value()};
        }
        const auto record   = co_await transitions->load(owner, source_run);
        const auto commands = co_await transitions->load_javascript_commands(owner, source_run);
        if (!record || commands.empty() || !commands.back().completed() ||
            commands.back().command().kind() != JavaScriptCommandKind::Checkpoint) {
            throw std::invalid_argument("no checkpoint handoff for run " + source_run);
        }
        const auto& checkpoint = commands.back();
        co_return ExactProgramHandoff{
            ExactProgramHandoffReference{source_run, record->journal_head(), checkpoint.coordinate_id(), checkpoint.id()},
            checkpoint.command().arguments().value("value", json())};
    }

    asio::awaitable<json> replace(json options, int call) {
        const auto source_run = options.at("runId").get<std::string>();
        const auto new_run    = options.at("newRunId").get<std::string>();
        const auto version    = options.at("version").get<std::string>();
        begin_run(new_run);
        std::optional<ProgramHandle> handle;
        try {
            handle.emplace(co_await replace_handle(std::move(options), call, source_run, new_run, version));
        } catch (...) {
            abandoned(new_run);
            throw;
        }
        co_await arrived(*handle);
        auto result = co_await handle->wait_async();
        settle(result);
        co_return result_json(result);
    }

    asio::awaitable<ProgramHandle> replace_handle(json options, int call, std::string source_run,
                                                  std::string new_run, std::string version) {
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        auto resolved = co_await resolve_handoff_for_replace(source_run);
        if (options.contains("journalHead"))
            resolved.reference.source_journal_head = options.at("journalHead").get<std::string>();
        if (options.contains("coordinateId"))
            resolved.reference.command_coordinate_id = options.at("coordinateId").get<std::string>();
        if (options.contains("entryId"))
            resolved.reference.command_entry_id = options.at("entryId").get<std::string>();

        const auto made = co_await transitions->load(owner, new_run);
        RunBudget base_budget = default_budget;
        if (made) {
            base_budget = made->invocation().budget;
        } else if (const auto lineage = co_await transitions->load_run_lineage(owner, source_run)) {
            base_budget = lineage->remaining_budget();
        }
        json input = options.value("input", json::object());
        if (input.is_object()) {
            if (!input.contains("handoff")) input["handoff"] = resolved.value;
            if (!input.contains("previous_run_id")) input["previous_run_id"] = source_run;
        }
        auto request = invocation(options, version, new_run,
                                  budget_from(options.value("budget", json::object()), base_budget),
                                  std::move(input));
        request.idempotency_key = "js-replace:" + new_run;
        request.validate();
        // The exact reference: a refused replacement leaves the source run, and the hold on it, as they were.
        if (handoffs_in_use.contains(source_run))
            throw std::invalid_argument("run " + source_run + " has a synthesis in progress on its handoff");
        auto handle = co_await runtime->replace_async(std::move(resolved.reference), std::move(request), std::move(sink));
        held_handoffs.erase(source_run);
        co_return handle;
    }

    asio::awaitable<json> migrate_graph(json options, int call) {
        const auto source_run = options.at("runId").get<std::string>();
        const auto new_run    = options.at("newRunId").get<std::string>();
        const auto version    = options.at("version").get<std::string>();
        begin_run(new_run);
        std::optional<ProgramHandle> handle;
        try {
            handle.emplace(co_await migrate_graph_handle(std::move(options), call, source_run, new_run, version));
        } catch (...) {
            abandoned(new_run);
            throw;
        }
        co_await arrived(*handle);
        auto result = co_await handle->wait_async();
        settle(result);
        co_return result_json(result);
    }

    asio::awaitable<ProgramHandle> migrate_graph_handle(json options, int call, std::string source_run,
                                                        std::string new_run, std::string version) {
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        auto source = co_await wait_for_handle(source_run);
        std::optional<GraphSemanticMigrationAdapter> adapter;
        // A semantic adapter only when the host asks for one, as in the C++ API.
        if (options.value("semanticAdapter", false)) {
            if (const auto record = co_await transitions->load(owner, source_run)) {
                const auto source_ver = co_await catalog->resolve_version_async(owner, record->program_version_id());
                const auto target_ver = co_await catalog->resolve_version_async(owner, version);
                if (source_ver && target_ver && source_ver->id() != target_ver->id()) {
                    const auto source_bundle = co_await programs->get_bundle(source_ver->bundle_id());
                    const auto target_bundle = co_await programs->get_bundle(target_ver->bundle_id());
                    if (source_bundle && target_bundle) {
                        adapter.emplace(GraphSemanticMigrationAdapter::prepare(
                            *source_ver, *source_bundle, *target_ver, *target_bundle));
                    }
                }
            }
        }
        ProgramGraphMigrationTarget target{version, new_run, std::move(sink), std::move(adapter)};
        co_return co_await runtime->migrate_graph_async(source, std::move(target));
    }

    asio::awaitable<json> synthesize_child(json options) {
        const auto parent_run   = options.at("runId").get<std::string>();
        const auto binding_name = options.contains("binding") ? options.at("binding").get<std::string>()
                                                              : options.at("bindingName").get<std::string>();
        const json source_spec  = options.contains("source") ? options.at("source") : options;
        const auto child_source = source_from(source_spec, "js:child:" + binding_name);

        auto parent_handle = co_await wait_for_handle(parent_run);
        auto* lease        = co_await acquire_handoff(parent_run);
        // The hold stays this synthesis's until it returns: no release, replacement or cancellation frees it meanwhile.
        if (!handoffs_in_use.insert(parent_run).second)
            throw std::invalid_argument("run " + parent_run + " has a synthesis in progress on its handoff");
        struct InUse {
            std::set<std::string>& runs;
            std::string            run;
            ~InUse() { runs.erase(run); }
        } in_use{handoffs_in_use, parent_run};
        const auto parent_record  = co_await transitions->load(owner, parent_run);
        const auto parent_lineage = co_await transitions->load_run_lineage(owner, parent_run);
        if (!parent_record || !parent_lineage) throw std::invalid_argument("no active run " + parent_run);
        const auto parent_version = co_await catalog->resolve_version_async(owner, parent_record->program_version_id());
        if (!parent_version) throw std::invalid_argument("no parent version for run " + parent_run);

        // What the child asks for (`budget`, `requestedCapabilities`, `requestedEffects`) and what the host allows it
        // (`grant`) are separate: the host's grant and semantic decision are never made from the request.
        if (!options.contains("budget"))
            throw std::invalid_argument("synthesizeChild() needs the child's budget");
        const auto child_budget = budget_from(options.at("budget"), RunBudget{});
        std::vector<std::string> requested_capabilities;
        std::vector<std::string> requested_effects;
        const auto read_tokens = [](const json& container, std::string_view key, std::vector<std::string>& out) {
            if (!container.is_object() || !container.contains(std::string(key))) return;
            const auto& arr = container.at(std::string(key));
            if (!arr.is_array()) throw std::invalid_argument(std::string(key) + " must be an array of strings");
            out.clear();
            for (const auto& item : arr) {
                if (!item.is_string()) throw std::invalid_argument(std::string(key) + " must be an array of strings");
                out.push_back(item.get<std::string>());
            }
        };
        read_tokens(source_spec, "capabilities", requested_capabilities);
        read_tokens(source_spec, "requestedCapabilities", requested_capabilities);
        read_tokens(options, "capabilities", requested_capabilities);
        read_tokens(options, "requestedCapabilities", requested_capabilities);
        read_tokens(source_spec, "effects", requested_effects);
        read_tokens(source_spec, "requestedEffects", requested_effects);
        read_tokens(options, "effects", requested_effects);
        read_tokens(options, "requestedEffects", requested_effects);

        ProgramSynthesisProposalData proposal_data;
        proposal_data.owner_scope            = owner;
        proposal_data.lineage_id             = parent_lineage->lineage_id();
        proposal_data.parent_run_id          = parent_run;
        proposal_data.source                 = child_source;
        proposal_data.requested_capabilities = requested_capabilities;
        proposal_data.requested_effects      = requested_effects;
        proposal_data.requested_budget       = child_budget;
        proposal_data.created_at_ms          = std::max<std::int64_t>(0, parent_record->updated_at_ms());
        const auto proposal = ProgramSynthesisProposal::create(std::move(proposal_data));

        if (!options.contains("grant") || !options.at("grant").is_object())
            throw std::invalid_argument("synthesizeChild() needs the host's grant");
        const bool has_decision = options.contains("semanticDecision") && !options.at("semanticDecision").is_null();
        if (!has_decision && !synthesis_validator)
            throw std::invalid_argument(
                "synthesizeChild() needs the host's semanticDecision or registerSynthesisValidator()");
        const auto& granted = options.at("grant");
        const auto  text    = [](const json& container, const char* key, const char* what) {
            if (!container.contains(key) || !container.at(key).is_string() ||
                container.at(key).get<std::string>().empty())
                throw std::invalid_argument(std::string(what) + "." + key + " must be a string");
            return container.at(key).get<std::string>();
        };
        ProgramChildSynthesisGrantData grant_data;
        grant_data.owner_scope                 = owner;
        grant_data.parent_run_id               = parent_run;
        grant_data.parent_program_version_id   = parent_version->id();
        grant_data.parent_policy_fingerprint   = parent_version->policy_snapshot().fingerprint();
        grant_data.lineage_id                  = parent_lineage->lineage_id();
        grant_data.expected_lineage_head_id    = parent_lineage->id();
        grant_data.template_identity           = text(granted, "templateIdentity", "grant");
        grant_data.reviewed_source_identity    = program_synthesis_source_identity(child_source);
        grant_data.semantic_validator_identity = text(granted, "validatorIdentity", "grant");
        grant_data.semantic_contract_identity  = text(granted, "contractIdentity", "grant");
        read_tokens(granted, "allowedCapabilities", grant_data.allowed_capabilities);
        read_tokens(granted, "allowedEffects", grant_data.allowed_effects);
        if (!granted.contains("childBudget"))
            throw std::invalid_argument("grant.childBudget must be a budget");
        grant_data.child_budget_ceiling        = budget_from(granted.at("childBudget"), RunBudget{});
        grant_data.minimum_execution_guarantee = parent_version->execution_guarantee();
        const auto grant = ProgramChildSynthesisGrant::create(std::move(grant_data));
        synthesis_grants.insert_or_assign(grant.id(), grant);
        if (has_decision) {
            const auto& decision = options.at("semanticDecision");
            if (!decision.is_object())
                throw std::invalid_argument("synthesizeChild() semanticDecision must be an object");
            if (!decision.contains("accepted") || !decision.at("accepted").is_boolean())
                throw std::invalid_argument("semanticDecision.accepted must be a boolean");
            json evidence = json::object();
            if (decision.contains("evidence") && !decision.at("evidence").is_null())
                evidence = decision.at("evidence");
            synthesis_decisions.insert_or_assign(
                proposal.id(),
                ProgramSynthesisSemanticDecision{text(decision, "validatorIdentity", "semanticDecision"),
                                                 text(decision, "contractIdentity", "semanticDecision"),
                                                 decision.at("accepted").get<bool>(),
                                                 std::move(evidence)});
        }

        struct DecisionUsed {
            std::map<std::string, ProgramSynthesisSemanticDecision>& decisions;
            std::string                                               proposal_id;
            ~DecisionUsed() { decisions.erase(proposal_id); }
        } decision_used{synthesis_decisions, proposal.id()};
        const auto record = co_await runtime->prepare_child_synthesis_async(
            owner, parent_handle, *lease, proposal, grant, binding_name);
        if (record.data().state != ProgramChildSynthesisState::Bound) {
            const std::string code =
                record.data().error_code.empty() ? "P_CHILD_SYNTHESIS_FAILED" : record.data().error_code;
            json witness = json{{"state", std::string(child_synthesis_state_name(record.data().state))},
                                {"record_id", record.id()},
                                {"proposal_id", proposal.id()},
                                {"grant_id", grant.id()}};
            if (record.data().artifacts.contains("evidence"))
                witness["evidence"] = record.data().artifacts.at("evidence");
            throw ProgramDiagnosticError(Diagnostic{
                CompilePhase::Seal,
                code,
                DiagnosticSeverity::Error,
                SourceCoordinate{"child_synthesis", "", std::nullopt},
                "Child synthesis did not bind an admitted child version: " + code,
                std::move(witness),
                {},
                std::nullopt});
        }
        const auto& artifacts = record.data().artifacts;
        std::string version_id;
        std::string bundle_id;
        if (artifacts.contains("version") && artifacts.at("version").contains("id"))
            version_id = artifacts.at("version").at("id").get<std::string>();
        if (artifacts.contains("bundle") && artifacts.at("bundle").contains("id"))
            bundle_id = artifacts.at("bundle").at("id").get<std::string>();
        co_return json{{"id", record.id()},
                       {"binding", binding_name},
                       {"state", std::string(child_synthesis_state_name(record.data().state))},
                       {"versionId", version_id},
                       {"bundleId", bundle_id},
                       {"proposalId", proposal.id()},
                       {"grantId", grant.id()}};
    }

    // What a fork of `source_run` that runs as `new_run` is given unless its options say otherwise. A fork that was made
    // before has the amounts its record holds, which are what the runtime compares a repeated request with: the same fork
    // again is the same fork, and the share is not taken a second time from what the forks since then left. Otherwise it is
    // half of what the paused run has left, read from the lineage, where a fork's share is debited.
    asio::awaitable<RunBudget> fork_share_of(const std::string& source_run, const std::string& new_run) {
        const auto made = co_await transitions->load(owner, new_run);
        if (made && made->fork_source_run_id() == source_run) co_return made->invocation().budget;
        const auto lineage = co_await transitions->load_run_lineage(owner, source_run);
        if (!lineage) throw std::invalid_argument("no run " + source_run + " to fork");
        co_return fork_share(lineage->remaining_budget());
    }

    asio::awaitable<ProgramHandle> fork_handle(json options, int call, std::string source_run, std::string new_run,
                                               std::string version) {
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        // The forks of one paused run take their turns, from the read of what the run has left to the publication of the
        // fork, which debits it. The share of each is then half of what the forks before it left, however many are made in
        // the same turn of the event loop and however long the stores take to answer.
        const auto turns = fork_turns_of(source_run);
        const auto turn  = co_await turns->lock_async();
        const auto share = co_await fork_share_of(source_run, new_run);
        auto request = invocation(options, version, new_run, budget_from(options.value("budget", json::object()), share),
                                  options.value("input", json::object()));
        request.message_sequence = 2;
        request.idempotency_key  = "js-fork:" + new_run;
        request.validate();
        ExactProgramCheckpointReference reference{source_run, options.at("checkpointId").get<std::string>()};
        ProgramResume answer{options.value("value", json()), options.value("traceId", "js:fork:" + new_run), sink,
                             options.at("pendingId").get<std::string>()};
        co_return co_await runtime->fork_async(std::move(reference), std::move(request), std::move(answer),
                                               std::move(sink));
    }

    asio::awaitable<json> cancel(std::string run_id) {
        if (handoffs_in_use.contains(run_id))
            throw std::invalid_argument("run " + run_id + " has a synthesis in progress on its handoff");
        held_handoffs.erase(run_id);
        const auto found = live.find(run_id);
        if (starting.contains(run_id)) {
            // A call is starting for this run. When an earlier attempt of the run is still executing, that is the run to
            // cancel (the call that is starting is a duplicate that the runtime will refuse); otherwise the cancellation
            // waits for the handle that the starting call is about to return.
            if (found == live.end() || found->second.try_result()) {
                cancel_requested.insert(run_id);
                co_return json(true);
            }
        }
        if (found == live.end()) co_return json(false);
        auto handle = found->second;
        const bool applied = co_await handle.cancel_async();
        co_return json(applied);
    }

    // What a run left in the transition store: its terminal result, or the state it is in.
    asio::awaitable<json> result(std::string run_id) {
        const auto record = co_await transitions->load(owner, run_id);
        if (!record) co_return json(nullptr);
        if (const auto terminal = record->terminal_result()) co_return result_json(*terminal);
        co_return json{{"runId", run_id}, {"status", "running"}};
    }
};

std::map<int, std::shared_ptr<Host>> hosts;
int last_host = 0;

// Runs `work` on the loop and reports its outcome through Module.ngProgramDone. `work` is a coroutine that holds the host.
void report(int call, asio::awaitable<json> work) {
    loop().spawn(std::move(work), [call](std::exception_ptr error, json value) {
        const auto text = error ? failure_json(error).dump() : json{{"ok", true}, {"result", std::move(value)}}.dump();
        ng_js_program_done(call, text.c_str());
    });
}

// Every operation is a free coroutine that owns its arguments and a reference to the host: the host stays alive for as long
// as a call is in flight, whatever JavaScript does with its object.
asio::awaitable<json> admit_op(std::shared_ptr<Host> host, json source) { co_return co_await host->admit(std::move(source)); }
asio::awaitable<json> start_op(std::shared_ptr<Host> host, json options, int call) {
    co_return co_await host->start(std::move(options), call);
}
asio::awaitable<json> resume_op(std::shared_ptr<Host> host, json options, int call) {
    co_return co_await host->resume(std::move(options), call);
}
asio::awaitable<json> fork_op(std::shared_ptr<Host> host, json options, int call) {
    co_return co_await host->fork(std::move(options), call);
}
asio::awaitable<json> handoff_op(std::shared_ptr<Host> host, json options) {
    co_return co_await host->handoff(std::move(options));
}
asio::awaitable<json> release_handoff_op(std::shared_ptr<Host> host, std::string run) {
    co_return co_await host->release_handoff(std::move(run));
}
asio::awaitable<json> replace_op(std::shared_ptr<Host> host, json options, int call) {
    co_return co_await host->replace(std::move(options), call);
}
asio::awaitable<json> migrate_graph_op(std::shared_ptr<Host> host, json options, int call) {
    co_return co_await host->migrate_graph(std::move(options), call);
}
asio::awaitable<json> synthesize_child_op(std::shared_ptr<Host> host, json options) {
    co_return co_await host->synthesize_child(std::move(options));
}
asio::awaitable<json> cancel_op(std::shared_ptr<Host> host, std::string run) { co_return co_await host->cancel(std::move(run)); }
asio::awaitable<json> result_op(std::shared_ptr<Host> host, std::string run) { co_return co_await host->result(std::move(run)); }

// Starts `make(host, ...)` for the Programs object `handle`, or fails the call at once.
template <typename Make>
int begin(int handle, int call, Make make) {
    try {
        const auto found = hosts.find(handle);
        if (found == hosts.end() || found->second->closed) throw std::invalid_argument("no such Programs object");
        report(call, make(found->second));
        return 0;
    } catch (const std::exception& error) {
        neograph::wasm::set_error(error.what());
    } catch (...) {
        neograph::wasm::set_error("unknown failure");
    }
    return -1;
}

}  // namespace

// `config_json`: {owner?, provider: {descriptor, api_key, model, ...} (as for ng_compile), limits?, budget?, javascript?}. `stores`: the
// handle of the host's stores object (see wasm/stores.mjs), or 0 for the in-memory stores. Returns a handle, or 0 with
// ng_error() set.
extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_create(const char* config_json, int stores) {
    try {
        auto host = std::make_shared<Host>(json::parse(config_json), stores);
        hosts.emplace(++last_host, std::move(host));
        return last_host;
    } catch (const std::exception& error) {
        neograph::wasm::set_error(error.what());
    } catch (...) {
        neograph::wasm::set_error("unknown failure");
    }
    return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_admit(int handle, int call, const char* source_json) {
    return begin(handle, call, [source = std::string(source_json)](std::shared_ptr<Host> host) {
        return admit_op(std::move(host), json::parse(source));
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_start(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json), call](std::shared_ptr<Host> host) {
        return start_op(std::move(host), json::parse(options), call);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_resume(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json), call](std::shared_ptr<Host> host) {
        return resume_op(std::move(host), json::parse(options), call);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_fork(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json), call](std::shared_ptr<Host> host) {
        return fork_op(std::move(host), json::parse(options), call);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_handoff(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json)](std::shared_ptr<Host> host) {
        return handoff_op(std::move(host), json::parse(options));
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_release_handoff(int handle, int call, const char* run_id) {
    return begin(handle, call, [run = std::string(run_id)](std::shared_ptr<Host> host) {
        return release_handoff_op(std::move(host), run);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_replace(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json), call](std::shared_ptr<Host> host) {
        return replace_op(std::move(host), json::parse(options), call);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_migrate_graph(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json), call](std::shared_ptr<Host> host) {
        return migrate_graph_op(std::move(host), json::parse(options), call);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_synthesize_child(int handle, int call, const char* options_json) {
    return begin(handle, call, [options = std::string(options_json)](std::shared_ptr<Host> host) {
        return synthesize_child_op(std::move(host), json::parse(options));
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_set_synthesis_validator(int handle, int validator_handle) {
    try {
        const auto found = hosts.find(handle);
        if (found == hosts.end() || found->second->closed) throw std::invalid_argument("no such Programs object");
#ifdef NEOGRAPH_JS_STORES
        if (validator_handle == 0) {
            found->second->synthesis_validator = {};
            return 0;
        }
        std::shared_ptr<const void> lease(new int(validator_handle), [](const void* held) {
            ng_js_stores_free(*static_cast<const int*>(held));
            delete static_cast<const int*>(held);
        });
        found->second->synthesis_validator =
            neograph::wasm::adapt_js_synthesis_validator(validator_handle, std::move(lease));
        return 0;
#else
        (void)validator_handle;
        throw std::invalid_argument("this module was built without JavaScript stores");
#endif
    } catch (const std::exception& error) {
        neograph::wasm::set_error(error.what());
    } catch (...) {
        neograph::wasm::set_error("unknown failure");
    }
    return -1;
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_cancel(int handle, int call, const char* run_id) {
    return begin(handle, call, [run = std::string(run_id)](std::shared_ptr<Host> host) {
        return cancel_op(std::move(host), run);
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE int ng_programs_result(int handle, int call, const char* run_id) {
    return begin(handle, call, [run = std::string(run_id)](std::shared_ptr<Host> host) {
        return result_op(std::move(host), run);
    });
}

// Stops what the object started and frees it once the calls in flight have finished.
extern "C" EMSCRIPTEN_KEEPALIVE void ng_programs_release(int handle) {
    const auto found = hosts.find(handle);
    if (found == hosts.end()) return;
    auto host    = found->second;
    host->closed = true;
    hosts.erase(found);
    host->synthesis_validator = {};
    host->held_handoffs.clear();
    for (const auto& id : host->starting) host->cancel_requested.insert(id);
    // A run that is executing is cancelled. One that waits at an interrupt stays as it is in the stores.
    for (auto& [id, run] : host->live)
        if (!run.try_result()) (void)run.cancel();
}
