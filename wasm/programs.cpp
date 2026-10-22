// The Program API of the JavaScript module (#556, #557): compile a Program from its source, admit it, start a run, observe
// its events, resume an interrupt, fork a paused run, read a result, cancel. wasm/neograph.mjs is its JavaScript side; this
// file is the C API it calls.
//
//   ng_programs_create(config, stores)   -> handle (0 on failure; ng_error() says why)
//   ng_programs_admit(handle, call, source)      compile and admit a Program
//   ng_programs_start(handle, call, options)     start a run
//   ng_programs_resume(handle, call, options)    resume an interrupted run
//   ng_programs_fork(handle, call, options)      fork an interrupted run at its checkpoint
//   ng_programs_cancel(handle, call, run)        cancel a run
//   ng_programs_result(handle, call, run)        read what a run left in the transition store
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
#include <neograph/program/program.h>
#include <neograph/program/store_call.h>

#include <emscripten.h>

#ifdef NEOGRAPH_JS_STORES
#include "js_stores.h"
#endif

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

AdmissionProfile make_profile(const RegistrySnapshot& registry) {
    AdmissionProfileBuilder builder;
    builder.id("js-module-profile")
        .semantic_version("1.0.0")
        .registry(registry)
        .mode(AdmissionMode::MultiTenant)
        .max_program_schema_version(LATEST_PROGRAM_SCHEMA_VERSION)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged)
        .allow_source_kind(SourceKind::JavaScript)
        .allow_source_kind(SourceKind::CppBuilder)
        .allow_effect_mode(EffectMode::Brokered);
    for (const auto& identity : registry.identities()) builder.allow_executable(identity);
    return std::move(builder).build();
}

PolicySnapshot make_policy(const AdmissionProfile& profile, const std::string& owner, const BudgetLimits& ceiling) {
    PolicySnapshotBuilder builder;
    builder.id("js-module-policy")
        .semantic_version("1.0.0")
        .owner_scope(owner)
        .admission_profile(profile)
        .budget_ceiling(ceiling)
        .minimum_execution_guarantee(ExecutionGuarantee::Unmanaged);
    return std::move(builder).build();
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

// One Programs object of JavaScript: the stores, the catalog that admits Programs into them, and the runtime that runs them.
struct Host {
    std::string owner;
    RegistrySnapshot registry = make_registry();
    AdmissionProfile profile  = make_profile(registry);
    std::optional<PolicySnapshot> policy;
    std::shared_ptr<ProgramCompiler> compiler;
    RunBudget default_budget;
    std::shared_ptr<ProgramTransitionStore> transitions;
    std::shared_ptr<ProgramCatalog>         catalog;
    std::unique_ptr<ProgramRuntime>         runtime;
    // The handles of the runs this object started, by run id, for cancel. A run that reached its end is forgotten.
    std::map<std::string, ProgramHandle> live;
    // A run whose start, resume or fork has not returned its handle yet cannot be cancelled through it. A cancel that
    // arrives meanwhile (an event handler that threw does that) is kept and applied once the handle exists.
    std::set<std::string> starting;
    std::set<std::string> cancel_requested;
    bool closed = false;

    Host(const json& config, int stores_handle) {
        owner = config.value("owner", "js:programs");
        const auto ceiling = budget_from(config.value("limits", json::object()),
                                         RunBudget{3'600'000, 100'000'000, 1'000'000'000, 16, 100'000, 100'000, 1, 4, 4});
        default_budget     = budget_from(config.value("budget", json::object()),
                                         RunBudget{60'000, 10'000, 1'000'000, 4, 32, 100, 0, 0, 0});
        policy.emplace(make_policy(profile, owner, limits_of(ceiling)));

        std::shared_ptr<ProgramStore>           programs;
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
        // until its time limit. The default limit of a minute is too long for that thread.
        JavaScriptCompileLimits javascript_limits;
        javascript_limits.max_wall_time_ms = 5'000;
        ProgramCompilerConfig compiler_config{"js-module/1", javascript_limits};
        compiler = std::make_shared<ProgramCompiler>(registry, std::move(compiler_config));

        CatalogConfig catalog_config{programs, registry, std::make_shared<EngineGenerationCache>(), "js-module/1"};
        catalog_config.capability_binder =
            neograph::program::detail::single_provider_binder(std::move(context), "js-host");
        catalog_config.module_store      = modules;
        catalog                          = std::make_shared<ProgramCatalog>(std::move(catalog_config));

        RuntimeConfig runtime_config;
        runtime_config.catalog             = catalog;
        runtime_config.checkpoints         = std::make_shared<graph::InMemoryCheckpointStore>();
        runtime_config.transitions         = transitions;
        runtime_config.task_graph_fragments = fragments;
        runtime_config.executor            = loop().executor();
        runtime = std::make_unique<ProgramRuntime>(std::move(runtime_config));
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
        if (cancel_requested.erase(run_id) != 0) {
            auto copy = handle;
            (void)co_await copy.cancel_async();
        }
    }
    void abandoned(const std::string& run_id) {
        starting.erase(run_id);
        cancel_requested.erase(run_id);
    }
    void settle(const ProgramResult& result) {
        // An interrupted run waits for its answer: its handle stays. Any other end forgets it.
        if (result.status() != ProgramTerminalStatus::Interrupted) live.erase(result.run_id());
    }

    asio::awaitable<json> admit(json source) {
        std::optional<ProgramSource> program;
        const std::string name = source.value("name", "js:program");
        if (source.contains("javascript"))
            program.emplace(ProgramSource::from_javascript(name, source.at("javascript").get<std::string>()));
        else if (source.contains("document")) {
            const auto& document = source.at("document");
            program.emplace(ProgramSource::from_cpp_builder(
                name, document.value("program_schema_version", PROGRAM_SCHEMA_VERSION_V1), document));
        } else {
            throw std::invalid_argument("a Program source is {javascript: text} or {document: object}");
        }
        const auto bundle = compiler->compile(*program);
        auto version = co_await catalog->admit_async(bundle, ProgramAdmission{owner, profile, *policy, {}});
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
        // From here until the handle exists the fork can be cancelled by name, whatever the lineage read below waits for.
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

    asio::awaitable<ProgramHandle> fork_handle(json options, int call, std::string source_run, std::string new_run,
                                               std::string version) {
        std::shared_ptr<ProgramEventSink> sink;
        if (options.value("stream", false)) sink = std::make_shared<JsEventSink>(call);
        // The fork takes its share of what the paused run has left now. The lineage is where a fork's share is debited, so
        // a second fork of the same pause takes half of the rest, not half of what the run had when it paused.
        const auto lineage = co_await transitions->load_run_lineage(owner, source_run);
        if (!lineage) throw std::invalid_argument("no run " + source_run + " to fork");
        const auto share = fork_share(lineage->remaining_budget());
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

// `config_json`: {owner?, provider: {descriptor, api_key, model, ...} (as for ng_compile), limits?, budget?}. `stores`: the
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
    for (const auto& id : host->starting) host->cancel_requested.insert(id);
    // A run that is executing is cancelled. One that waits at an interrupt stays as it is in the stores.
    for (auto& [id, run] : host->live)
        if (!run.try_result()) (void)run.cancel();
}
