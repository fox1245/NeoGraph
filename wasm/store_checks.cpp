// What the store conformance suites cannot reach: the adapters of wasm/js_stores.h driven by a JavaScript store whose
// methods settle Promises in later tasks of the host's event loop. A Promise settles only when this module has returned
// to JavaScript, so nothing here waits: wasm/test/js_stores_test.mjs starts a scenario, runs the host's tasks one at a
// time between awaits of its own, and reads the outcome.
//
//   ng_check_begin(scenario, plain, other)  starts a scenario on a HostLoop over a host that runs a task only when asked
//   ng_check_step()                         runs one task of the host; 0 when none is ready
//   ng_check_state()                        0 running, 1 finished, 2 failed
//   ng_check_report()                       the scenario's findings, as JSON
//   ng_check_end()                          destroys the loop, and with it every coroutine that has not finished
//
// `plain` and `other` are handles of stores objects registered with the bridge (wasm/stores.mjs). What each scenario
// expects of them is in the scenario's comment.
#include "js_stores.h"
#include "memory_stores.h"

#include <neograph/async/host_loop.h>
#include <neograph/graph/checkpoint.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#include <neograph/program/transition_store.h>

#include <runtime/host.h>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/this_coro.hpp>

#include <emscripten.h>

#include <chrono>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

using neograph::json;
namespace async   = neograph::async;
namespace graph   = neograph::graph;
namespace program = neograph::program;
namespace wasm    = neograph::wasm;

namespace {

// The host's own loop: it queues what it is given and runs it when the driver asks, as a JavaScript host runs its
// microtasks and timers.
class StepHost final : public sp::runtime::Executor {
public:
    void post(Task task) override { ready_.push_back(std::move(task)); }
    Timer schedule(sp::runtime::SteadyTime at, Task task) override {
        const auto id = ++last_timer_;
        timers_.emplace(id, Timed{at, std::move(task)});
        return id;
    }
    void cancel(Timer id) noexcept override { timers_.erase(id); }
    sp::runtime::SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
    sp::runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
    bool in_thread() const noexcept override { return running_ != 0; }
    bool host_pumped() const noexcept override { return true; }
    void shutdown() noexcept override {}

    bool step() {
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.at <= now()) {
                ready_.push_back(std::move(it->second.task));
                it = timers_.erase(it);
            } else {
                ++it;
            }
        }
        if (ready_.empty()) return false;
        auto task = std::move(ready_.front());
        ready_.pop_front();
        ++running_;
        try {
            task();
        } catch (...) {
            --running_;
            throw;
        }
        --running_;
        return true;
    }
    std::size_t ready() const { return ready_.size(); }

private:
    struct Timed {
        sp::runtime::SteadyTime at;
        Task                    task;
    };
    std::deque<Task>       ready_;
    std::map<Timer, Timed> timers_;
    Timer                  last_timer_ = 0;
    unsigned               running_    = 0;
};

class OneNode final : public graph::GraphNode {
public:
    explicit OneNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        graph::NodeOutput out;
        out.writes.push_back(graph::ChannelWrite{"value", json("hosted")});
        co_return out;
    }

private:
    std::string name_;
};

// A one-node Program, admitted into the Program store it is given.
struct Fixture {
    program::RegistrySnapshot                registry;
    program::AdmissionProfile                profile;
    program::PolicySnapshot                  policy;
    std::shared_ptr<program::ProgramStore>   store;
    std::shared_ptr<program::ProgramCatalog> catalog;
    std::shared_ptr<graph::CheckpointStore>  checkpoints = std::make_shared<graph::InMemoryCheckpointStore>();
    program::ProgramBundle                   bundle;

    static program::ExecutableManifest manifest(program::ExecutableKind kind, std::string name, char implementation) {
        return program::ExecutableManifest{{kind, std::move(name), "1.0.0", "sha256:" + std::string(64, implementation)},
                                           program::EffectMode::Brokered, "attestation:store-checks", {}, {}, {}};
    }

    explicit Fixture(std::shared_ptr<program::ProgramStore> program_store)
        : registry(make_registry()),
          profile(make_profile(registry)),
          policy(make_policy(profile)),
          store(std::move(program_store)),
          catalog(std::make_shared<program::ProgramCatalog>(
              program::CatalogConfig{store, registry, std::make_shared<program::EngineGenerationCache>(),
                                     "store-checks/v1"})),
          bundle(compile(registry)) {}

    static program::RegistrySnapshot make_registry() {
        program::RegistrySnapshotBuilder builder;
        builder.add_node(
            manifest(program::ExecutableKind::Node, "store-one-node", '1'),
            [](const std::string& name, const json&, const graph::NodeContext&) { return std::make_unique<OneNode>(name); },
            json{{"type", "object"}}, json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
        builder.add_reducer(manifest(program::ExecutableKind::Reducer, "store-overwrite", '3'),
                            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    }

    static program::AdmissionProfile make_profile(const program::RegistrySnapshot& registry) {
        program::AdmissionProfileBuilder profile_builder;
        profile_builder.id("store-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(program::AdmissionMode::MultiTenant)
            .max_program_schema_version(program::PROGRAM_SCHEMA_VERSION_V4)
            .allow_source_kind(program::SourceKind::CppBuilder)
            .allow_effect_mode(program::EffectMode::Brokered);
        for (const auto& identity : registry.identities()) profile_builder.allow_executable(identity);
        return std::move(profile_builder).build();
    }

    static program::PolicySnapshot make_policy(const program::AdmissionProfile& profile) {
        program::PolicySnapshotBuilder policy_builder;
        policy_builder.id("store-policy")
            .semantic_version("1.0.0")
            .owner_scope("store:tenant")
            .admission_profile(profile)
            .budget_ceiling(program::BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32});
        return std::move(policy_builder).build();
    }

    static program::ProgramBundle compile(const program::RegistrySnapshot& registry) {
        const json document{
            {"program_schema_version", 1},
            {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
            {"root",
             json{{"op", "call_core"},
                  {"name", "main"},
                  {"definition",
                   json{{"schema_version", 1},
                        {"name", "main"},
                        {"channels", json{{"value", json{{"reducer", "store-overwrite"}, {"initial", ""}}}}},
                        {"nodes", json{{"work", json{{"type", "store-one-node"}}}}},
                        {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                               json{{"from", "work"}, {"to", "__end__"}}})},
                        {"conditional_edges", json::array()}}}}},
            {"declared_budget_requirements",
             json::array({
                 json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
                 json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
                 json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
                 json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
                 json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
                 json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                 json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                 json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                 json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
             })}};
        program::ProgramCompiler compiler(registry, {"store-checks/v1"});
        return compiler.compile(program::ProgramSource::from_cpp_builder("store:checks", 1, document));
    }

    asio::awaitable<program::ProgramVersion> admit() {
        co_return co_await catalog->admit_async(bundle, program::ProgramAdmission{"store:tenant", profile, policy, {}});
    }

    program::RuntimeConfig config(std::shared_ptr<program::ProgramTransitionStore> transitions) const {
        return program::RuntimeConfig{catalog, checkpoints, {}, std::move(transitions), 1};
    }
};

program::ProgramInvocation invocation(std::string trace) {
    return program::ProgramInvocation{json::object(), program::RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0},
                                      std::move(trace), {}};
}

// ---------------------------------------------------------------------------------------------------------------

struct Run {
    std::shared_ptr<StepHost>    host = std::make_shared<StepHost>();
    std::optional<async::HostLoop> loop;
    wasm::JsStoreSet             plain, other;
    std::uint64_t                steps = 0;
    int                          state = 0;  // 0 running, 1 finished, 2 failed
    json                         report = json::object();
    asio::cancellation_signal    cancel;
    std::uint64_t                steps_at_call = 0;
};

std::unique_ptr<Run> run;
std::string          report_text;
std::string          bridge_text;

// The class of a C++ exception, as the contract names it.
std::string class_of(const std::exception& error) {
    if (dynamic_cast<const std::invalid_argument*>(&error)) return "invalid_argument";
    if (dynamic_cast<const std::out_of_range*>(&error)) return "out_of_range";
    if (dynamic_cast<const std::domain_error*>(&error)) return "domain_error";
    if (dynamic_cast<const std::length_error*>(&error)) return "length_error";
    if (dynamic_cast<const std::logic_error*>(&error)) return "logic_error";
    if (dynamic_cast<const std::system_error*>(&error)) return "system_error";
    if (dynamic_cast<const std::runtime_error*>(&error)) return "runtime_error";
    return "exception";
}

json describe(std::exception_ptr error) {
    json out = json::object();
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        out["type"]    = class_of(failure);
        out["message"] = std::string(failure.what());
    } catch (...) {
        out["type"]    = "unknown";
        out["message"] = "";
    }
    return out;
}

// One call of a scenario: what it returned or threw. A value is never a default.
template <class T>
asio::awaitable<void> attempt(json& rows, const char* name, asio::awaitable<T> call) {
    json row    = json::object();
    row["call"] = name;
    try {
        (void)co_await std::move(call);
        row["outcome"] = "value";
    } catch (...) {
        row["outcome"]   = "exception";
        auto detail      = describe(std::current_exception());
        row["type"]      = detail["type"];
        row["message"]   = detail["message"];
    }
    rows.push_back(row);
}

// Calls a spread of operations whose arguments need no stored value: a result of each kind (an optional record, a list,
// a struct of values, an enum, a bool). The stores object decides what each call does.
asio::awaitable<void> probe(const wasm::JsStoreSet& stores, json& rows) {
    static const std::vector<std::string> none;
    co_await attempt(rows, "transitions.load", stores.transitions->load("store:tenant", "run-1"));
    co_await attempt(rows, "transitions.latest", stores.transitions->latest("store:tenant", "run-1"));
    co_await attempt(rows, "transitions.loadRunPublicationHead",
                     stores.transitions->load_run_publication_head("store:tenant", "run-1"));
    co_await attempt(rows, "transitions.loadEvents", stores.transitions->load_events("store:tenant", "run-1", 0));
    co_await attempt(rows, "transitions.loadEffectiveRuntimeState",
                     stores.transitions->load_effective_runtime_state("store:tenant", "run-1"));
    co_await attempt(rows, "programs.getVersion", stores.programs->get_version("store:tenant", "version-1"));
    co_await attempt(rows, "programs.compareActivate",
                     stores.programs->compare_activate("store:tenant", 0, "version-1", "policy-1"));
    co_await attempt(rows, "programs.listVersions", stores.programs->list_versions("store:tenant"));
    co_await attempt(rows, "programs.collectGarbage", stores.programs->collect_garbage("store:tenant", none));
    program::ProgramCoreToolGrantContext context{"store:tenant", "version-1", "run-1", "op-1", 1, "binding-1"};
    co_await attempt(rows, "grants.revoke", stores.grants->revoke(context));
    co_await attempt(rows, "grants.admit",
                     stores.grants->admit({"store:tenant", "version-1", "run-1", "op-1", 1, "binding-1", "grant-1"}));
    co_await attempt(rows, "transitions.loadJavascriptCommands",
                     stores.transitions->load_javascript_commands("store:tenant", "run-1", 0));
    co_await attempt(rows, "programs.getActivation", stores.programs->get_activation("store:tenant"));
    co_await attempt(rows, "fragments.load", stores.fragments->load("fragment-1"));
    co_await attempt(rows, "fragments.findPublishedLineage",
                     stores.fragments->find_published_lineage("store:tenant", "run-1", "operation-1"));
    co_await attempt(rows, "modules.get", stores.modules->get("store:tenant", "module-1"));
}

// Runs the Program of the fixture over `programs` and `transitions` and returns its result.
asio::awaitable<program::ProgramResult> run_program(Run& state, Fixture& fixture,
                                                     std::shared_ptr<program::ProgramTransitionStore> transitions,
                                                     std::string trace, program::ProgramVersion version) {
    auto config     = fixture.config(std::move(transitions));
    config.executor = state.loop->executor();
    program::ProgramRuntime runtime(std::move(config));
    co_return co_await runtime.run_async("store:tenant", version, invocation(std::move(trace)));
}

// -----------------------------------------------------------------------------------------------------------------
// Scenario 0. `other` returns a Promise from every method, which settles in a later task of the JavaScript event loop.
// A Program is run over `plain` first, so that `other` has a run to read. The coroutine reads it through `other`: it
// must suspend, wake no turn while the Promise is pending, and go on in a turn of the loop, on the loop's executor.
asio::awaitable<int> scenario_pending(Run& state) {
    Fixture fixture(state.plain.programs);
    const auto version = co_await fixture.admit();
    const auto result  = co_await run_program(state, fixture, state.plain.transitions, "pending-seed", version);
    const std::string run_id = result.run_id();

    auto& out = state.report;
    out["seed_status"]    = result.status() == program::ProgramTerminalStatus::Completed ? "completed" : "other";
    out["in_turn_before"] = async::in_host_turn();
    state.steps_at_call   = state.steps;
    const auto before     = wasm::js_store_stats();
    const auto record     = co_await state.other.transitions->load("store:tenant", run_id);
    const auto after      = wasm::js_store_stats();
    out["in_turn_after"]  = async::in_host_turn();
    out["on_loop_executor"] = (co_await asio::this_coro::executor) == state.loop->executor();
    out["record_found"]   = record.has_value();
    out["record_matches"] = record && record->run_id() == run_id;
    out["steps_across_call"] = static_cast<unsigned long long>(state.steps - state.steps_at_call);
    out["pending_calls"]  = static_cast<unsigned long long>(after.pending - before.pending);
    out["completed_calls"] = static_cast<unsigned long long>(after.completed - before.completed);
    // A value that is not there is a null, not a failure, and the next call goes on in the same turn as this one.
    const auto missing = co_await state.other.transitions->load("store:tenant", "no-such-run");
    out["missing_is_null"] = !missing.has_value();
    co_return 0;
}

// Scenario 1. `plain` returns values, not Promises. Ten calls of every kind complete in the turn that made them: the
// host runs exactly one task for the whole scenario (the one that starts the coroutine).
asio::awaitable<int> scenario_immediate(Run& state) {
    json rows = json::array();
    for (int index = 0; index < 10; ++index) co_await probe(state.plain, rows);
    state.report["calls"] = static_cast<unsigned long long>(rows.size());
    state.report["rows"]  = rows;
    co_return 0;
}

// Scenarios 2 and 3. `other` throws, rejects, or returns a malformed value from the operations of probe(); the report
// lists what each call did.
asio::awaitable<int> scenario_probe(Run& state) {
    json rows = json::array();
    co_await probe(state.other, rows);
    state.report["rows"] = rows;
    co_return 0;
}

// Scenario 4. `other` has some methods that return values and some that return Promises, and the coroutine uses the
// ones after the others: a Program runs over it, and the run is read back through both kinds.
asio::awaitable<int> scenario_mixed(Run& state) {
    Fixture fixture(state.other.programs);
    const auto version = co_await fixture.admit();
    const auto result  = co_await run_program(state, fixture, state.other.transitions, "mixed", version);
    state.report["status"] = result.status() == program::ProgramTerminalStatus::Completed ? "completed" : "other";
    const auto stored      = co_await state.other.programs->get_version("store:tenant", version.id());
    state.report["version_read_back"] = stored.has_value() && stored->id() == version.id();
    const auto record      = co_await state.other.transitions->load("store:tenant", result.run_id());
    state.report["run_read_back"] = record.has_value() && record->run_id() == result.run_id();
    const auto stats = wasm::js_store_stats();
    state.report["pending_calls"]   = static_cast<unsigned long long>(stats.pending);
    state.report["immediate_calls"] = static_cast<unsigned long long>(stats.immediate);
    co_return 0;
}

// Scenarios 5 and 6. `other` returns a Promise that stays pending until JavaScript settles it. The coroutine waits on
// it; in scenario 5 the loop is destroyed under it, in scenario 6 the coroutine is cancelled. The Promise settles
// afterwards, and its outcome must be dropped.
asio::awaitable<int> scenario_wait(Run& state) {
    state.report["started"] = true;
    try {
        (void)co_await state.other.transitions->load("store:tenant", "run-1");
        state.report["resumed"] = true;
    } catch (...) {
        state.report["cancelled"] = describe(std::current_exception());
        throw;
    }
    co_return 0;
}

// Scenarios 8 and 9. A Program over adapters whose methods all return Promises (8) or whose transition store rejects
// every call (9), with the Program store (9) answering at once so that the version is admitted.
asio::awaitable<int> scenario_program(Run& state, bool rejecting) {
    Fixture fixture(rejecting ? state.plain.programs : state.other.programs);
    const auto version = co_await fixture.admit();
    state.report["admitted"]   = !version.id().empty();
    state.report["version_id"] = version.id();
    try {
        const auto result = co_await run_program(state, fixture, state.other.transitions,
                                                 rejecting ? "rejecting" : "promised", version);
        state.report["status"] = result.status() == program::ProgramTerminalStatus::Completed ? "completed" : "failed";
        state.report["run_id"] = result.run_id();
        if (result.status() == program::ProgramTerminalStatus::Completed) {
            const auto stored = co_await state.other.transitions->load("store:tenant", result.run_id());
            state.report["run_read_back"] = stored.has_value();
            const auto head = co_await state.other.transitions->load_run_publication_head("store:tenant", result.run_id());
            state.report["head_read_back"] = head.has_value();
            const auto stored_version = co_await state.other.programs->get_version("store:tenant", version.id());
            state.report["version_read_back"] = stored_version.has_value();
            const auto resolved = co_await fixture.catalog->resolve_version_async("store:tenant", version.id());
            state.report["catalog_read"] = resolved.has_value();
        }
    } catch (...) {
        state.report["status"]    = "exception";
        auto detail               = describe(std::current_exception());
        state.report["type"]      = detail["type"];
        state.report["message"]   = detail["message"];
    }
    const auto stats = wasm::js_store_stats();
    state.report["pending_calls"]   = static_cast<unsigned long long>(stats.pending);
    state.report["completed_calls"] = static_cast<unsigned long long>(stats.completed);
    state.report["waiting"]         = static_cast<unsigned long long>(stats.waiting);
    co_return 0;
}

asio::awaitable<int> dispatch(int scenario) {
    auto& state = *run;
    switch (scenario) {
    case 0: co_return co_await scenario_pending(state);
    case 1: co_return co_await scenario_immediate(state);
    case 2:
    case 3: co_return co_await scenario_probe(state);
    case 4: co_return co_await scenario_mixed(state);
    case 5:
    case 6: co_return co_await scenario_wait(state);
    case 8: co_return co_await scenario_program(state, false);
    case 9: co_return co_await scenario_program(state, true);
    default: throw std::invalid_argument("no such scenario");
    }
}

}  // namespace

extern "C" {

// Starts a scenario. Returns 0, or -1 when it could not start (ng_check_report() says why).
EMSCRIPTEN_KEEPALIVE int ng_check_begin(int scenario, int plain, int other) {
    try {
        run = std::make_unique<Run>();
        run->loop.emplace(run->host);
        if (plain) run->plain = wasm::adapt_js_stores(plain);
        if (other) run->other = wasm::adapt_js_stores(other);
        wasm::set_js_store_executor(run->loop->executor());
        auto done = [state = run.get()](std::exception_ptr error, int) {
            if (error) {
                state->report["failure"] = describe(error);
                state->state             = 2;
            } else {
                state->state = 1;
            }
        };
        if (scenario == 6) {
            asio::co_spawn(run->loop->executor(), dispatch(scenario),
                           asio::bind_cancellation_slot(run->cancel.slot(), std::move(done)));
            run->loop->wake();
        } else {
            run->loop->spawn(dispatch(scenario), std::move(done));
        }
        return 0;
    } catch (const std::exception& error) {
        report_text = describe(std::current_exception()).dump();
        return -1;
    }
}

// Runs one task of the host. Returns 1 when one ran, 0 when none was ready.
EMSCRIPTEN_KEEPALIVE int ng_check_step() {
    if (!run) return 0;
    try {
        if (!run->host->step()) return 0;
        ++run->steps;
        return 1;
    } catch (...) {
        run->report["step_failure"] = describe(std::current_exception());
        run->state                  = 2;
        return 0;
    }
}

EMSCRIPTEN_KEEPALIVE int ng_check_state() { return run ? run->state : 0; }
EMSCRIPTEN_KEEPALIVE int ng_check_steps() { return run ? static_cast<int>(run->steps) : 0; }
// 1 when the host has no task ready and the loop has no turn scheduled: nothing is waiting to run.
EMSCRIPTEN_KEEPALIVE int ng_check_idle() { return run && run->host->ready() == 0 && run->loop->idle() ? 1 : 0; }

// Emits a terminal cancellation to the coroutine of scenario 6.
EMSCRIPTEN_KEEPALIVE void ng_check_cancel() {
    if (!run) return;
    run->cancel.emit(asio::cancellation_type::terminal);
    run->loop->wake();  // a cancellation from outside the loop schedules the turn that delivers it
}

// What the bridge has done since the module started, as JSON; it outlives a scenario.
EMSCRIPTEN_KEEPALIVE const char* ng_check_bridge() {
    const auto stats = wasm::js_store_stats();
    json       bridge = json::object();
    bridge["calls"]     = static_cast<unsigned long long>(stats.calls);
    bridge["immediate"] = static_cast<unsigned long long>(stats.immediate);
    bridge["pending"]   = static_cast<unsigned long long>(stats.pending);
    bridge["completed"] = static_cast<unsigned long long>(stats.completed);
    bridge["dropped"]   = static_cast<unsigned long long>(stats.dropped);
    bridge["waiting"]   = static_cast<unsigned long long>(stats.waiting);
    bridge_text         = bridge.dump();
    return bridge_text.c_str();
}

EMSCRIPTEN_KEEPALIVE const char* ng_check_report() {
    if (run) {
        run->report["host_steps"] = static_cast<unsigned long long>(run->steps);
        report_text               = run->report.dump();
    }
    return report_text.c_str();
}

// Destroys the loop and the scenario. The executor given to the bridge goes first.
EMSCRIPTEN_KEEPALIVE void ng_check_end() {
    wasm::set_js_store_executor({});
    run.reset();
}

// Adapts a stores object and says what happened: {"ok": true, "members": [...]} or {"ok": false, "type", "message"}.
EMSCRIPTEN_KEEPALIVE const char* ng_check_adapt(int handle) {
    json out = json::object();
    try {
        const auto set = wasm::adapt_js_stores(handle);
        json       members = json::array();
        if (set.transitions) members.push_back(json("transitions"));
        if (set.programs) members.push_back(json("programs"));
        if (set.grants) members.push_back(json("grants"));
        if (set.fragments) members.push_back(json("fragments"));
        if (set.modules) members.push_back(json("modules"));
        out["ok"]      = true;
        out["members"] = members;
    } catch (...) {
        out["ok"] = false;
        auto detail = describe(std::current_exception());
        out["type"]    = detail["type"];
        out["message"] = detail["message"];
    }
    report_text = out.dump();
    return report_text.c_str();
}

}  // extern "C"
