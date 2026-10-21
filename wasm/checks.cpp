// What differs on a 32-bit WebAssembly host, checked where it differs. The program exits 0 when every check
// passes and prints the name of each one that does not. No network or model call is performed.
//
//   nonce      The managed-budget journal draws its nonce from the SDK's entropy source (getentropy here).
//   deadline   A managed budget with a deadline needs a clock origin a restart can recover. This host has none,
//              so the run is refused with a typed error.
//   store      The in-memory store runs a managed budget on the calling thread. A synchronous checkpoint store
//              needs a worker thread; a single-thread build has none, so the operation is refused with a typed
//              error instead of waiting for it.
//   plugin     A native Program control plugin is refused with a typed error (the v1 ABI is a 64-bit layout).
//   transport  The libcurl constructor of SchemaProvider is refused with the SDK's typed error (no transport here).
//   blocking   A synchronous run that reaches a provider on a host-pumped executor is refused with the SDK's typed
//              reason, and nothing is sent. Waiting for the host's event loop would hang the one thread.
//   program    A ProgramRuntime without an executor is refused with a typed error (it has no thread to run on). On the
//              executor of a host loop, the synchronous calls are refused naming the awaitable form, and a one-node
//              Program with in-memory stores runs to its result through the awaitable calls, in the host's turns.

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/configuration_failure.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM
#include <neograph/program/native.h>
#include <neograph/program/program.h>
#include <neograph/program/store.h>
#include <neograph/program/store_call.h>
#include <neograph/program/transition_store.h>
#endif
#if defined(NEOGRAPH_WASM_CHECKS_PROGRAM) && defined(NEOGRAPH_WASM_CHECKS_LLM)
#define NEOGRAPH_WASM_CHECKS_PROGRAM_RUNTIME 1
#endif
#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM_RUNTIME
#include <neograph/async/host_loop.h>
#endif
#ifdef NEOGRAPH_WASM_CHECKS_LLM
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <runtime/host.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace neograph;
using namespace neograph::graph;

namespace {

int failures = 0;

void check(bool ok, const char* name, const char* detail = "") {
    std::printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", name, *detail ? ": " : "", detail);
    if (!ok) ++failures;
}

bool lower_hex_64(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

class DoubleNode : public GraphNode {
public:
    explicit DoubleNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<NodeOutput> run(NodeInput in) override {
        auto seed = in.state.get("seed");
        int v = seed.is_number() ? seed.get<int>() : 0;
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"doubled", json(v * 2)});
        co_return out;
    }
private:
    std::string name_;
};

std::unique_ptr<GraphEngine> make_engine(std::shared_ptr<CheckpointStore> store = nullptr) {
    static const bool registered = [] {
        NodeFactory::instance().register_type(
            "doubler",
            [](const std::string& name, const json&, const NodeContext&) -> std::unique_ptr<GraphNode> {
                return std::make_unique<DoubleNode>(name);
            });
        return true;
    }();
    (void)registered;
    json defn = {
        {"name", "wasm_checks"},
        {"channels", {
            {"seed",    {{"reducer", "overwrite"}}},
            {"doubled", {{"reducer", "overwrite"}}},
        }},
        {"nodes", {{"d", {{"type", "doubler"}}}}},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "d"}},
            {{"from", "d"},         {"to", "__end__"}},
        })},
    };
    auto engine = GraphEngine::compile(defn, NodeContext());
    engine->set_checkpoint_store(store ? std::move(store) : std::make_shared<InMemoryCheckpointStore>());
    return engine;
}

RunConfig budgeted_run(const char* thread_id) {
    RunConfig cfg;
    cfg.thread_id         = thread_id;
    cfg.input             = json{{"seed", 21}};
    cfg.max_steps         = 5;
    cfg.model_token_budget = 1000;
    return cfg;
}

void check_nonce() {
    try {
        InMemoryCheckpointStore store;
        ManagedBudgetLeaseScope scope;
        scope.owner_scope     = "wasm-checks";
        scope.thread_id       = "nonce-1";
        scope.graph_identity  = "wasm-checks";
        scope.original_ceiling = 100;
        const auto first = store.acquire_managed_budget_lease(scope, "", "");
        scope.thread_id = "nonce-2";
        const auto second = store.acquire_managed_budget_lease(scope, "", "");
        check(first && second && lower_hex_64(first->actor_id()) && lower_hex_64(first->bank_generation()),
              "nonce: a lease carries a 256-bit actor and generation");
        check(first && second && first->actor_id() != second->actor_id() &&
                  first->bank_generation() != second->bank_generation(),
              "nonce: two leases do not share a nonce");
    } catch (const std::exception& error) {
        check(false, "nonce: a lease is issued", error.what());
    }
}

void check_budget_on_the_in_memory_store() {
    // The in-memory store runs its managed-budget operations on the calling thread, so a budgeted graph runs here.
    try {
        auto engine = make_engine();
        const auto result = engine->run(budgeted_run("budget-1"));
        check(result.output["channels"]["doubled"]["value"].get<int>() == 42,
              "store: a managed budget runs on the in-memory store");
    } catch (const std::exception& error) {
        check(false, "store: a managed budget runs on the in-memory store", error.what());
    }
}

void check_synchronous_store() {
    // A store that is not the in-memory one hands its operations to a pool of worker threads. This build has none,
    // so the run is refused where it used to wait for a thread that never started.
    struct SynchronousStore final : InMemoryCheckpointStore {};
    try {
        auto engine = make_engine(std::make_shared<SynchronousStore>());
        engine->run(budgeted_run("budget-3"));
        check(false, "store: a synchronous store is refused", "the run was accepted");
    } catch (const SynchronousCheckpointStoreUnavailable& error) {
        check(dynamic_cast<const ConfigurationFailure*>(&error) != nullptr,
              "store: a synchronous store is refused with a typed error");
    } catch (const std::exception& error) {
        check(false, "store: a synchronous store is refused with a typed error", error.what());
    }
}

void check_budget_with_deadline() {
    RunMetadata metadata;
    metadata.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    try {
        auto engine = make_engine();
        engine->run(budgeted_run("budget-2"), metadata);
        check(false, "deadline: a managed budget with a deadline is refused", "the run was accepted");
    } catch (const ManagedBudgetDeadlineClockUnavailable& error) {
        check(true, "deadline: a managed budget with a deadline is refused with a typed error");
        check(dynamic_cast<const ConfigurationFailure*>(&error) != nullptr &&
                  dynamic_cast<const std::runtime_error*>(&error) != nullptr,
              "deadline: the refusal is a configuration failure, never retried");
    } catch (const std::exception& error) {
        check(false, "deadline: a managed budget with a deadline is refused with a typed error", error.what());
    }
}

#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM
int32_t plugin_invoke(void*, const neograph_program_native_invoke_request_v1*,
                      neograph_program_native_completion_v1, void*) {
    return NEOGRAPH_PROGRAM_NATIVE_INVOKE_REJECTED;
}
void plugin_cancel(void*, uint64_t) {}
void plugin_destroy(void*) {}

void check_native_plugin() {
    check(!program::NATIVE_CONTROLS_AVAILABLE, "plugin: native controls are declared unavailable");
    neograph_program_native_binding_v1 binding{};
    binding.abi_version = NEOGRAPH_PROGRAM_NATIVE_ABI_V1;
    binding.struct_size = sizeof(binding);
    binding.invoke      = plugin_invoke;
    binding.cancel      = plugin_cancel;
    binding.destroy     = plugin_destroy;
    try {
        program::NativeControlBinding::create(binding, program::NativeControlMetadata{});
        check(false, "plugin: a native control binding is refused", "the binding was accepted");
    } catch (const program::NativeControlUnavailable& error) {
        check(true, "plugin: a native control binding is refused with a typed error");
        check(dynamic_cast<const ConfigurationFailure*>(&error) != nullptr &&
                  dynamic_cast<const std::runtime_error*>(&error) != nullptr,
              "plugin: the refusal is a configuration failure, never retried");
    } catch (const std::exception& error) {
        check(false, "plugin: a native control binding is refused with a typed error", error.what());
    }
}
#endif

#ifdef NEOGRAPH_WASM_CHECKS_LLM
// A host that never runs: it declares itself pumped, queues what it is given and starts no request.
class PumpedExecutor final : public sp::runtime::Executor {
public:
    void post(Task) override {}
    Timer schedule(sp::runtime::SteadyTime, Task) override { return 0; }
    void cancel(Timer) noexcept override {}
    sp::runtime::SteadyTime now() const noexcept override { return std::chrono::steady_clock::now(); }
    sp::runtime::WallTime wall_now() const noexcept override { return std::chrono::system_clock::now(); }
    bool in_thread() const noexcept override { return false; }
    bool host_pumped() const noexcept override { return true; }
    void shutdown() noexcept override {}
};
class CountingTransport final : public sp::runtime::HostTransport {
public:
    std::unique_ptr<sp::runtime::HostAttempt> start(sp::transport::HttpRequest, sp::transport::Callbacks) override {
        ++starts;
        throw std::logic_error("a refused call must not send");
    }
    void shutdown() noexcept override {}
    int starts = 0;
};

std::string provider_failure_reason(std::exception_ptr error) {
    for (int depth = 0; error && depth < 8; ++depth) {
        try {
            std::rethrow_exception(error);
        } catch (const NodeExecutionError& failure) {
            error = failure.cause();
        } catch (const ProviderFailure& failure) {
            return std::get<sp::Failure>(*failure.outcome()).error.reason;
        } catch (...) {
            return {};
        }
    }
    return {};
}

void check_libcurl_constructor() {
    // This build has no libcurl transport: the constructor that would use it names what is missing.
    try {
        auto loaded = sp::descriptor::load(
            R"({"descriptor_version":1,"revision":1,"id":"wasm-checks","family":"openai.chat","connection":{"base_url":"https://api.example.test","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
        llm::SchemaProvider provider(std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
                                     sp::runtime::Options::with_api_key("key"));
        check(false, "transport: the libcurl constructor is refused", "the provider was built");
    } catch (const sp::descriptor::ConfigError& error) {
        check(error.pointer == "/transport", "transport: the libcurl constructor is refused with the SDK's typed error");
    } catch (const std::exception& error) {
        check(false, "transport: the libcurl constructor is refused with the SDK's typed error", error.what());
    }
}

void check_blocking_run() {
    try {
        auto loaded = sp::descriptor::load(
            R"({"descriptor_version":1,"revision":1,"id":"wasm-checks","family":"openai.chat","connection":{"base_url":"https://api.example.test","paths":{"buffered":"/v1/chat/completions","streaming":"/v1/chat/completions"}}})");
        auto transport = std::make_shared<CountingTransport>();
        auto provider = std::make_shared<llm::SchemaProvider>(
            std::get<sp::descriptor::ValidatedDescriptor>(std::move(loaded)),
            sp::runtime::Options::with_api_key("key"), std::make_shared<PumpedExecutor>(), transport);
        json defn = {
            {"name", "wasm_checks_llm"},
            {"channels", {{"messages", {{"reducer", "append"}}}}},
            {"nodes", {{"llm", {{"type", "llm_call"}}}}},
            {"edges", json::array({{{"from", "__start__"}, {"to", "llm"}}, {{"from", "llm"}, {"to", "__end__"}}})},
        };
        NodeContext context;
        context.provider = provider;
        context.model = "model";
        auto engine = GraphEngine::compile(defn, context);
        RunConfig cfg;
        cfg.thread_id = "blocking-1";
        cfg.provider_messages = std::vector<sp::Message>{portable_message(ChatMessage{"user", "hello"})};
        std::exception_ptr failure;
        try {
            engine->run(cfg);
        } catch (...) {
            failure = std::current_exception();
        }
        check(failure != nullptr, "blocking: a synchronous run on a host-pumped provider is refused");
        check(provider_failure_reason(failure) == "runtime.blocking.host_pumped",
              "blocking: the refusal carries the SDK's reason");
        check(transport->starts == 0, "blocking: nothing is sent");
    } catch (const std::exception& error) {
        check(false, "blocking: a synchronous run on a host-pumped provider is refused", error.what());
    }
}
#endif

#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM_RUNTIME
// The host's own loop: it queues what it is given and runs it when main() asks, as a JavaScript host runs its
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
        try { task(); } catch (...) { --running_; throw; }
        --running_;
        return true;
    }

private:
    struct Timed { sp::runtime::SteadyTime at; Task task; };
    std::deque<Task> ready_;
    std::map<Timer, Timed> timers_;
    Timer last_timer_ = 0;
    unsigned running_ = 0;
};

class OneNode final : public GraphNode {
public:
    explicit OneNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<NodeOutput> run(NodeInput) override {
        NodeOutput out;
        out.writes.push_back(ChannelWrite{"value", json("hosted")});
        co_return out;
    }
private:
    std::string name_;
};

struct ProgramFixture {
    program::RegistrySnapshot                       registry;
    program::AdmissionProfile                       profile;
    program::PolicySnapshot                         policy;
    std::shared_ptr<program::InMemoryProgramStore>  store = std::make_shared<program::InMemoryProgramStore>();
    std::shared_ptr<program::ProgramCatalog>        catalog;
    std::shared_ptr<CheckpointStore>                checkpoints = std::make_shared<InMemoryCheckpointStore>();
    std::shared_ptr<program::ProgramTransitionStore> transitions =
        std::make_shared<program::InMemoryProgramTransitionStore>();
    program::ProgramVersion                         version;

    static program::ExecutableManifest manifest(program::ExecutableKind kind, std::string name, char implementation) {
        return program::ExecutableManifest{{kind, std::move(name), "1.0.0", "sha256:" + std::string(64, implementation)},
                                           program::EffectMode::Brokered, "attestation:wasm-checks", {}, {}, {}};
    }

    ProgramFixture()
        : registry(make_registry()),
          profile(make_profile(registry)),
          policy(make_policy(profile)),
          version(admit()) {}

    static program::RegistrySnapshot make_registry() {
        program::RegistrySnapshotBuilder builder;
        builder.add_node(
            manifest(program::ExecutableKind::Node, "wasm-one-node", '1'),
            [](const std::string& name, const json&, const NodeContext&) { return std::make_unique<OneNode>(name); },
            json{{"type", "object"}},
            json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
        builder.add_reducer(manifest(program::ExecutableKind::Reducer, "wasm-overwrite", '3'),
                            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    }

    static program::AdmissionProfile make_profile(const program::RegistrySnapshot& registry) {
        program::AdmissionProfileBuilder profile_builder;
        profile_builder.id("wasm-profile")
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
        policy_builder.id("wasm-policy")
            .semantic_version("1.0.0")
            .owner_scope("wasm:tenant")
            .admission_profile(profile)
            .budget_ceiling(program::BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32});
        return std::move(policy_builder).build();
    }

    program::ProgramVersion admit() {
        catalog = std::make_shared<program::ProgramCatalog>(
            program::CatalogConfig{store, registry, std::make_shared<program::EngineGenerationCache>(),
                                   "wasm-checks/v1"});
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
                        {"channels", json{{"value", json{{"reducer", "wasm-overwrite"}, {"initial", ""}}}}},
                        {"nodes", json{{"work", json{{"type", "wasm-one-node"}}}}},
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
        program::ProgramCompiler compiler(registry, {"wasm-checks/v1"});
        const auto bundle = compiler.compile(program::ProgramSource::from_cpp_builder("wasm:checks", 1, document));
        return catalog->admit(bundle, program::ProgramAdmission{"wasm:tenant", profile, policy, {}});
    }

    program::RuntimeConfig config() const {
        return program::RuntimeConfig{catalog, checkpoints, {}, transitions, 1};
    }
};

// Reads what the run left in the stores through their awaitable operations, as a host's coroutine does. co_spawn builds a
// default result for a failure, and these records have none: they are kept on the side.
asio::awaitable<int> read_back_awaited(std::shared_ptr<program::ProgramTransitionStore> transitions,
                                       std::shared_ptr<program::ProgramStore> store, std::string run_id,
                                       std::string version_id, std::optional<program::ProgramRunRecord>& run,
                                       std::optional<program::ProgramVersion>& version) {
    run     = co_await transitions->load("wasm:tenant", run_id);
    version = co_await store->get_version(version_id);
    co_return 0;
}

void check_program_runtime() {
    try {
        ProgramFixture fixture;
        try {
            program::ProgramRuntime runtime(fixture.config());
            check(false, "program: a runtime without an executor is refused", "the runtime was built");
        } catch (const program::ProgramExecutorRequired& error) {
            check(dynamic_cast<const ConfigurationFailure*>(&error) != nullptr,
                  "program: a runtime without an executor is refused with a typed error");
        }

        auto host = std::make_shared<StepHost>();
        async::HostLoop loop(host);
        auto config     = fixture.config();
        config.executor = loop.executor();
        program::ProgramRuntime runtime(std::move(config));
        const program::ProgramInvocation invocation{
            json::object(), program::RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}, "wasm-trace", {}};

        try {
            (void)runtime.start("wasm:tenant", fixture.version, invocation);
            check(false, "program: a synchronous start is refused on a host loop", "the call was accepted");
        } catch (const program::ProgramBlockingCallRefused& error) {
            check(error.alternative() == "ProgramRuntime::start_async",
                  "program: a synchronous start is refused, naming the awaitable form");
        }

        bool done = false;
        std::exception_ptr failure;
        std::optional<program::ProgramResult> result;
        loop.spawn(runtime.run_async("wasm:tenant", fixture.version, invocation),
                   [&](std::exception_ptr error, program::ProgramResult value) {
                       failure = error;
                       if (!error) result = std::move(value);
                       done = true;
                   });
        for (int turns = 0; !done && turns < 100000; ++turns) host->step();
        if (failure) std::rethrow_exception(failure);
        check(done && result && result->status() == program::ProgramTerminalStatus::Completed,
              "program: a one-node Program completes on the host loop with in-memory stores");
        check(result && result->output()["channels"]["value"]["value"] == "hosted",
              "program: the node's output is the Program's result");

        // The stores are awaitable: a coroutine of the host reads the run back in a host turn, and each operation of
        // the in-memory stores completes without suspending.
        if (result) {
            bool                                  read = false;
            std::exception_ptr                    read_failure;
            std::optional<program::ProgramRunRecord> run;
            std::optional<program::ProgramVersion>   version;
            loop.spawn(read_back_awaited(fixture.transitions, fixture.store, result->run_id(), fixture.version.id(),
                                         run, version),
                       [&](std::exception_ptr error, int) {
                           read_failure = error;
                           read         = true;
                       });
            for (int turns = 0; !read && turns < 100000; ++turns) host->step();
            if (read_failure) std::rethrow_exception(read_failure);
            check(read && run && run->continuation().state == program::ContinuationState::Completed,
                  "program: the awaitable transition store returns the run the Program completed");
            check(version && version->id() == fixture.version.id(),
                  "program: the awaitable Program store returns the version that was admitted");
        }

        // A caller that cannot suspend uses the blocking view, and the view refuses to wait for a store on a host loop.
        program::BlockingStorePolicy policy;
        policy.await_synchronous_stores = true;
        policy.refuse                   = [] { return std::string("the executor is run by a host's event loop"); };
        program::BlockingProgramTransitionStore view(fixture.transitions, policy);
        try {
            (void)view.load("wasm:tenant", "run-1");
            check(false, "program: a blocking store view refuses to wait on a host loop", "the call was accepted");
        } catch (const program::ProgramBlockingCallRefused& error) {
            check(dynamic_cast<const ConfigurationFailure*>(&error) != nullptr,
                  "program: a blocking store view refuses to wait on a host loop, with a typed error");
        }
    } catch (const std::exception& error) {
        check(false, "program: a Program runs on a host loop", error.what());
    }
}
#endif

}  // namespace

int main() {
    check_nonce();
    check_budget_on_the_in_memory_store();
    check_synchronous_store();
    check_budget_with_deadline();
#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM
    check_native_plugin();
#endif
#ifdef NEOGRAPH_WASM_CHECKS_LLM
    check_libcurl_constructor();
    check_blocking_run();
#endif
#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM_RUNTIME
    check_program_runtime();
#endif
    std::printf("%s\n", failures == 0 ? "all checks passed" : "checks failed");
    return failures == 0 ? 0 : 1;
}
