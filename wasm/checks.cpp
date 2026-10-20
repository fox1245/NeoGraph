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

#include <neograph/graph/checkpoint.h>
#include <neograph/graph/configuration_failure.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/state.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#ifdef NEOGRAPH_WASM_CHECKS_PROGRAM
#include <neograph/program/native.h>
#endif
#ifdef NEOGRAPH_WASM_CHECKS_LLM
#include <neograph/llm/schema_provider.h>
#include <descriptor/descriptor.h>
#include <runtime/host.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
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
    std::printf("%s\n", failures == 0 ? "all checks passed" : "checks failed");
    return failures == 0 ? 0 : 1;
}
