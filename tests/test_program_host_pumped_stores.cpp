// A Program lifecycle over stores that complete only when a host's event loop is pumped (#556, #557).
//
// The host here is a manual executor that the test steps from its own thread, as a JavaScript host runs its microtasks:
// nothing waits, nothing runs on another thread, and a store call that suspends is resumed by a later task of the same
// loop. Every operation of the runtime is awaited on it. The stores are the five a Program reads (the Program store, the
// transition store, the Core tool grant store, the task graph fragment store and the module store), each behind a
// wrapper that takes one turn of the loop per call (tests/fixtures/host_pumped_stores.h).
//
// What each test pins: a thread that waited for such a store could never finish the wait, so the runtime must await it
// on every path it reads or writes one on: starting a run (the catalog), a node's Core tool grant, the terminal
// publication of a run, a child's start and its completion, a resume, a fork, and a read of the catalog or of the module
// store. The synchronous forms stay what they were for a threaded host and are refused, before they touch a store,
// where they would wait for a turn only the calling thread can run.
#include "fixtures/fork_share.h"
#include "fixtures/host_pumped_stores.h"
#include "fixtures/manual_executor.h"
#include "fixtures/thread_census.h"
#include "fixtures/typed_provider.h"
#include "fixtures/vanishing_checkpoint_store.h"
#include "program_expansion_fixture.h"

#include <neograph/graph/node.h>
#include <neograph/program/program.h>
#include <neograph/tool_effect_broker.h>

#include "provider_binding.h"

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
#include <neograph/program/sqlite_transition_store.h>
#endif

#include <asio/detached.hpp>
#include <asio/this_coro.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "fixtures/store_handle.h"

namespace {

using namespace neograph;
using namespace neograph::program;
using namespace neograph::program::test_support;
using namespace neograph::program_test;
using neograph::host_test::HostedRuntime;
using namespace std::chrono_literals;

const std::string kOwner = "tenant:runtime";

std::atomic<int> approvals_asked{0};
std::atomic<int> approvals_run{0};
std::atomic<int> model_calls{0};
std::atomic<int> tool_runs{0};
std::atomic<int> child_runs{0};
std::atomic<int> effect_runs{0};
// Runs inside the child node, before it returns: a test switches stores to host turns from here.
std::function<void()> on_child_node;

// ---- Nodes ----

// Asks for approval until it is resumed, then writes the answer.
class ApproveNode final : public graph::GraphNode {
public:
    explicit ApproveNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        ++approvals_run;
        if (!in.ctx.resume_value) {
            ++approvals_asked;
            throw graph::NodeInterrupt("approval required", json{{"request", "approve"}});
        }
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", in.ctx.resume_value->at("decision")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// Asks for the result of an effect that may have run, until it is resumed with one, then writes the result.
class EffectNode final : public graph::GraphNode {
public:
    explicit EffectNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        ++approvals_run;
        ++effect_runs;
        if (!in.ctx.resume_value) {
            throw graph::NodeInterrupt(
                "capability result required",
                json{{"__neograph_program_pending_kind", "effect"},
                     {"call_id", "call-effect-1"},
                     {"result_schema", json{{"type", "object"}}},
                     {"effect", json{{"effect_id", "effect-search-1"},
                                     {"idempotency", "non_idempotent"},
                                     {"tool", "search"},
                                     {"arguments", json{{"query", "neo"}}}}},
                     {"expires_at_unix_ms", 4102444800000ULL}});
        }
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", in.ctx.resume_value->at("result")});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class ProbeTool final : public Tool {
public:
    ChatTool get_definition() const override { return {"probe", "A tool the Core grant gates", json{{"type", "object"}}}; }
    std::string get_name() const override { return "probe"; }
    std::string execute(const json&) override {
        ++tool_runs;
        return "ok";
    }
};

// Calls a tool through the gate of the run's Core tool grant: without the grant the call is denied and the tool is not run.
class ToolNode final : public graph::GraphNode {
public:
    explicit ToolNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        ProbeTool tool;
        ToolGateContext gate_context;
        gate_context.thread_id = in.ctx.thread_id;
        gate_context.step      = in.ctx.step;
        auto execution         = graph::make_tool_execution_context(in.ctx);
        std::vector<ToolCall> calls;
        calls.push_back(ToolCall{"call-1", "probe", "{}"});
        std::vector<Tool*> tools{&tool};
        (void)co_await dispatch_tool_calls(std::move(calls), std::move(tools), in.ctx.tool_gate,
                                           std::move(gate_context), std::move(execution));
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"acted", tool_runs.load()});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

class ChildNode final : public graph::GraphNode {
public:
    explicit ChildNode(std::string name) : name_(std::move(name)) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput) override {
        ++child_runs;
        if (on_child_node) on_child_node();
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"value", "child-done"});
        co_return output;
    }
    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

// ---- The registry, the Programs and the fake model ----

const ExecutableIdentity kProviderIdentity{ExecutableKind::Provider, "fixture-provider", "1.0.0", digest('1')};

ExecutableManifest host_manifest(ExecutableIdentity identity, std::vector<ExecutableIdentity> requires_ = {}) {
    return ExecutableManifest{std::move(identity), EffectMode::Brokered, "attestation:test", {}, {}, std::move(requires_)};
}

RegistrySnapshot host_registry(ExecutionGuarantee = ExecutionGuarantee::Strict) {
    RegistrySnapshotBuilder builder;
    builder.add_provider(host_manifest(kProviderIdentity), ProviderMetadata{json::object(), json::object()});
    builder.add_core_llm_call(
        host_manifest({ExecutableKind::Node, "llm_call", "1.0.0", digest('2')}, {kProviderIdentity}), {});
    builder.add_node(
        host_manifest({ExecutableKind::Node, "host-approve", "1.0.0", digest('3')}),
        [](const std::string& name, const json&, const graph::NodeContext&) { return std::make_unique<ApproveNode>(name); },
        json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
    builder.add_node(
        host_manifest({ExecutableKind::Node, "host-effect", "1.0.0", digest('8')}),
        [](const std::string& name, const json&, const graph::NodeContext&) { return std::make_unique<EffectNode>(name); },
        json{{"type", "object"}}, json{{"writes", json::array({"value"})}});
    builder.add_node(
        host_manifest({ExecutableKind::Node, "host-tool", "1.0.0", digest('4')}),
        [](const std::string& name, const json&, const graph::NodeContext&) { return std::make_unique<ToolNode>(name); },
        json{{"type", "object"}}, json{{"writes", json::array({"acted"})}});
    // The node of every child Program, under the names the shared documents use.
    builder.add_node(
        host_manifest({ExecutableKind::Node, "runtime-completed", "1.0.0", digest('5')}),
        [](const std::string& name, const json&, const graph::NodeContext&) { return std::make_unique<ChildNode>(name); },
        json{{"type", "object"}}, json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
    builder.add_reducer(host_manifest({ExecutableKind::Reducer, "runtime-overwrite", "1.0.0", digest('6')}),
                        [](const json&, const json& incoming) { return json(incoming); });
    builder.add_reducer(host_manifest({ExecutableKind::Reducer, "append", "1.0.0", digest('7')}),
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

using Fixture = BasicAdmittedRuntime<host_registry>;

std::shared_ptr<Provider> fake_model() {
    return std::make_shared<neograph::test::LocalProvider>(
        [](ProviderRequest, const PreparedProviderRequest&, const auto&) -> asio::awaitable<sp::runtime::Result> {
            ++model_calls;
            co_return neograph::test::success("an answer", neograph::test::usage(21, 3, 24));
        },
        "fake-upstream", neograph::test::bounded_client("fixture-model", 100));
}

// approve -> ask the model -> act: a Program that pauses once, calls the model and gates a tool call.
json review_document() {
    json definition{{"schema_version", 1}, {"name", "main"}, {"conditional_edges", json::array()}};
    definition["channels"] = {{"messages", {{"reducer", "append"}}},
                              {"value", {{"reducer", "runtime-overwrite"}, {"initial", ""}}},
                              {"acted", {{"reducer", "runtime-overwrite"}, {"initial", 0}}}};
    definition["nodes"] = {{"approve", {{"type", "host-approve"}}},
                           {"ask", {{"type", "llm_call"}}},
                           {"act", {{"type", "host-tool"}}}};
    const auto edge = [](const char* from, const char* to) { return json{{"from", from}, {"to", to}}; };
    definition["edges"] = json::array({edge("__start__", "approve"), edge("approve", "ask"), edge("ask", "act"),
                                       edge("act", "__end__")});
    return json{{"program_schema_version", 2U},
                {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
                {"declared_budget_requirements", budget_requirements()}};
}

// A chain of nodes of the given types; every node writes `value`.
json chain_document(const std::vector<std::pair<std::string, std::string>>& nodes) {
    json definition{{"schema_version", 1}, {"name", "main"}, {"conditional_edges", json::array()}};
    definition["channels"] = {{"value", {{"reducer", "runtime-overwrite"}, {"initial", ""}}}};
    definition["nodes"]    = json::object();
    json edges             = json::array();
    std::string previous   = "__start__";
    for (const auto& [name, type] : nodes) {
        definition["nodes"][name] = {{"type", type}};
        edges.push_back(json{{"from", previous}, {"to", name}});
        previous = name;
    }
    edges.push_back(json{{"from", previous}, {"to", "__end__"}});
    definition["edges"] = std::move(edges);
    return json{{"program_schema_version", 2U},
                {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
                {"declared_budget_requirements", budget_requirements()}};
}

// Two approvals in a row: the run pauses twice, in two attempts.
json two_pause_document() { return chain_document({{"approve-one", "host-approve"}, {"approve-two", "host-approve"}}); }

// One effect whose outcome the host reconciles.
json effect_document() { return chain_document({{"effect", "host-effect"}}); }

// act: a Program that only gates a tool call.
json tool_only_document() {
    json definition{{"schema_version", 1}, {"name", "main"}, {"conditional_edges", json::array()}};
    definition["channels"] = {{"acted", {{"reducer", "runtime-overwrite"}, {"initial", 0}}}};
    definition["nodes"]    = {{"act", {{"type", "host-tool"}}}};
    definition["edges"]    = json::array({json{{"from", "__start__"}, {"to", "act"}},
                                          json{{"from", "act"}, {"to", "__end__"}}});
    return json{{"program_schema_version", 2U},
                {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
                {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", std::move(definition)}}},
                {"declared_budget_requirements", budget_requirements()}};
}

// The child of the expanded task graph; its declared maxima are what the module link grants it.
json child_program_document() {
    auto document = program_document("runtime-completed");
    document["declared_budget_requirements"][0]["maximum"] = 3333;
    document["declared_budget_requirements"][1]["maximum"] = 333;
    document["declared_budget_requirements"][2]["maximum"] = 333;
    document["declared_budget_requirements"][5]["maximum"] = 6;
    document["root"]["definition"]["channels"]["item"] =
        json{{"reducer", "runtime-overwrite"}, {"initial", json::object()}};
    return document;
}

struct LinkHelper {
    test_support::ProgramStoreHandle store;
    ChildBindingRegistry&            bindings;
    void bind_child(const ProgramVersion& parent, std::string name, ProgramRuntimeChildBinding binding) {
        bindings.bind(parent.id(), std::move(name), std::move(binding));
    }
};

RunBudget run_budget() { return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}; }
// The child Program accepts no more than its module link grants it.
RunBudget child_budget() { return RunBudget{3333, 333, 333, 1, 1, 6, 0, 0, 0}; }

RunInvocation top_level(std::string version_id, std::string run_id) {
    RunInvocation invocation;
    invocation.owner_scope        = kOwner;
    invocation.agent_id           = "host-pumped-test";
    invocation.program_version_id = std::move(version_id);
    invocation.run_id             = run_id;
    invocation.budget             = run_budget();
    invocation.input              = json{{"messages", json::array({json{{"role", "user"}, {"content", "question"}}})}};
    invocation.message_sequence   = 1;
    invocation.idempotency_key    = "host-pumped:" + run_id;
    invocation.correlation_id     = "trace-" + run_id;
    invocation.validate();
    return invocation;
}

// ---- The host: plain stores for setup, wrapped stores for the run ----

// A store of Core tool grants in memory, for the host that has no durable one.
class MemoryGrantStore final : public SyncProgramCoreToolGrantStore {
public:
    ProgramCoreToolGrantAdmission admit_sync(const ProgramCoreToolGrantRecord& record) override {
        std::lock_guard lock(mutex_);
        const auto      key   = key_of(record.owner_scope, record.program_version_id, record.run_id,
                                       record.operation_id, record.attempt);
        const auto      found = records_.find(key);
        if (found == records_.end()) {
            records_.emplace(key, record);
            return ProgramCoreToolGrantAdmission::Admitted;
        }
        const auto& held = found->second;
        return held.grant_id == record.grant_id && held.binding_fingerprint == record.binding_fingerprint
                   ? ProgramCoreToolGrantAdmission::AlreadyPresent
                   : ProgramCoreToolGrantAdmission::Conflict;
    }
    std::optional<ProgramCoreToolGrantRecord> load_sync(const ProgramCoreToolGrantContext& context) const override {
        std::lock_guard lock(mutex_);
        const auto      found = records_.find(key_of(context.owner_scope, context.program_version_id, context.run_id,
                                                      context.operation_id, context.attempt));
        if (found == records_.end()) return std::nullopt;
        return found->second;
    }
    bool revoke_sync(const ProgramCoreToolGrantContext& context) override {
        std::lock_guard lock(mutex_);
        return records_.erase(key_of(context.owner_scope, context.program_version_id, context.run_id,
                                     context.operation_id, context.attempt)) != 0;
    }

private:
    using Key = std::tuple<std::string, std::string, std::string, std::string, std::uint64_t>;
    static Key key_of(std::string_view owner, std::string_view version, std::string_view run,
                      std::string_view operation, std::uint64_t attempt) {
        return {std::string(owner), std::string(version), std::string(run), std::string(operation), attempt};
    }
    mutable std::mutex           mutex_;
    std::map<Key, ProgramCoreToolGrantRecord> records_;
};

// The broker of a granted tool call: it runs the tool.
class RunningBroker final : public ToolEffectBroker {
public:
    asio::awaitable<ToolExecutionResult> execute(ToolEffectIdentity, Tool& tool, json arguments,
                                                 ToolExecutionContext context) override {
        co_return co_await context.controller->execute_result_async(tool, std::move(arguments), context);
    }
};

// One switch per store, so that a test can pump the one whose path it pins.
struct PumpedStores {
    std::shared_ptr<HostTurns> transitions = std::make_shared<HostTurns>(false);
    std::shared_ptr<HostTurns> programs    = std::make_shared<HostTurns>(false);
    std::shared_ptr<HostTurns> grants      = std::make_shared<HostTurns>(false);
    std::shared_ptr<HostTurns> fragments   = std::make_shared<HostTurns>(false);
    std::shared_ptr<HostTurns> modules     = std::make_shared<HostTurns>(false);
    void pump_all(bool pumped = true) {
        for (const auto& turns : {transitions, programs, grants, fragments, modules}) turns->set_pumped(pumped);
    }
    std::uint64_t operations() const {
        return transitions->operations() + programs->operations() + grants->operations() + fragments->operations() +
               modules->operations();
    }
};

class HostPumpedProgramHost {
public:
    // The model every llm_call node of the Programs answers from.
    static std::optional<graph::NodeContext> bound_model() {
        graph::NodeContext context;
        context.provider                            = fake_model();
        context.model                               = "fixture-model";
        context.provider_controls.max_output_tokens = 64;
        return context;
    }

    // The stores the runtime reads through a wrapper that takes turns of the loop. A test wraps only the store whose
    // path it pins, so that the others complete at once and the path is the only thing that can wait or be refused.
    enum Wrap : unsigned { Programs = 1, Transitions = 2, Grants = 4, Fragments = 8, Modules = 16, All = 31 };

    // `javascript` makes the catalog admit a Program with a JavaScript main(), which a Program of documents does not need.
    // `checkpoints` replaces the in-memory store of Core checkpoints, `configure` changes the runtime's configuration
    // before the runtime is made, and `journal` is the transition store the runtime reads, behind the wrapper when
    // Transitions is pumped; empty: in memory.
    explicit HostPumpedProgramHost(unsigned wrap = All, bool javascript = false,
                                   std::shared_ptr<CheckpointStore>  checkpoints = {},
                                   std::function<void(RuntimeConfig&)> configure   = {},
                                   std::shared_ptr<ProgramTransitionStore> journal = {})
        : fixture(1, std::move(checkpoints), std::move(journal), {}, javascript ? ExecutionGuarantee::Unmanaged : ExecutionGuarantee::Strict,
                  javascript) {
        fixture.runtime.reset();
        approvals_asked = 0;
        approvals_run   = 0;
        model_calls     = 0;
        tool_runs       = 0;
        child_runs      = 0;
        effect_runs     = 0;
        on_child_node   = nullptr;

        LinkHelper link{fixture.store, *fixture.child_bindings};
        link_child_versions(link, parent, child, BudgetLimits{3333, 333, 333, 1, 1, 6, 0, 0, 0});

        // The runtime reads and writes the wrapped stores through their wrappers, and the others as they are.
        wrapped_programs    = std::make_shared<HostPumpedProgramStore>(fixture.store.shared(), stores.programs);
        wrapped_transitions = std::make_shared<HostPumpedTransitionStore>(fixture.journal.shared(), stores.transitions);
        wrapped_fragments   = std::make_shared<HostPumpedFragmentStore>(fixture.task_graph_fragments.shared(), stores.fragments);
        wrapped_grants      = std::make_shared<HostPumpedGrantStore>(inner_grants, stores.grants);
        wrapped_modules     = std::make_shared<HostPumpedModuleStore>(inner_modules, stores.modules);
        const auto used_programs    = (wrap & Programs) ? wrapped_programs : std::shared_ptr<ProgramStore>(fixture.store.shared());
        const auto used_transitions = (wrap & Transitions) ? wrapped_transitions : std::shared_ptr<ProgramTransitionStore>(fixture.journal.shared());
        const auto used_fragments   = (wrap & Fragments) ? wrapped_fragments : std::shared_ptr<TaskGraphFragmentStore>(fixture.task_graph_fragments.shared());
        const auto used_grants      = (wrap & Grants) ? wrapped_grants : std::shared_ptr<ProgramCoreToolGrantStore>(inner_grants);
        const auto used_modules     = (wrap & Modules) ? wrapped_modules : std::shared_ptr<ModuleStore>(inner_modules);
        catalog                     = make_catalog(used_programs, used_modules);

        RuntimeConfig config;
        config.catalog                = catalog;
        config.checkpoints            = fixture.checkpoints;
        config.transitions            = used_transitions;
        config.child_binding_resolver = [bindings = fixture.child_bindings](
                                            std::string_view owner, std::string_view parent_version, std::string_view name) {
            return bindings->resolve(owner, parent_version, name);
        };
        config.task_graph_fragments       = used_fragments;
        config.task_graph_policy_resolver = [policy = fixture.expansion](std::string_view owner, std::string_view,
                                                                         std::string_view)
            -> std::optional<TaskGraphExpansionPolicy> {
            if (owner != kOwner) return std::nullopt;
            return *policy;
        };
        config.core_tool_grant_awaitable_resolver = make_durable_core_tool_grant_awaitable_resolver(
            used_grants, [broker = std::make_shared<RunningBroker>()](const ProgramCoreToolGrantRecord& record)
                                -> std::optional<ProgramCoreToolGrant> {
                ProgramCoreToolGrant granted;
                granted.owner_scope         = record.owner_scope;
                granted.program_version_id  = record.program_version_id;
                granted.run_id              = record.run_id;
                granted.operation_id        = record.operation_id;
                granted.attempt             = record.attempt;
                granted.binding_fingerprint = record.binding_fingerprint;
                granted.grant_id            = record.grant_id;
                granted.gate = [](ToolCall, ToolGateContext) -> asio::awaitable<ToolDecision> {
                    co_return ToolDecision::allow();
                };
                granted.controller    = std::make_shared<ToolExecutionController>();
                granted.effect_broker = broker;
                return granted;
            });
        config.executor = hosted.loop.executor();
        if (configure) configure(config);
        runtime = std::make_unique<ProgramRuntime>(std::move(config));
    }

    ~HostPumpedProgramHost() {
        // The runtime cancels what is left, and the host's turns finish it before the loop goes.
        runtime.reset();
        if (drain_on_destroy) hosted.host->run_until([] { return false; }, 50ms);
    }

    // False: the loop goes with the work it still has queued, as a host that shuts down does.
    bool drain_on_destroy = true;

    // Lets the tool be used by `run_id`, in each of its first four attempts.
    void grant_tool(const std::string& run_id, const ProgramVersion& version) {
        const auto fingerprint = capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings);
        for (std::uint64_t attempt = 1; attempt <= 4; ++attempt) {
            const ProgramCoreToolGrantRecord record{kOwner, version.id(), run_id, "root", attempt, fingerprint,
                                                    "grant-" + run_id};
            if (inner_grants->admit_sync(record) == ProgramCoreToolGrantAdmission::Conflict)
                throw std::logic_error("grant conflict");
        }
    }

    // Runs `script` on the host loop and pumps the loop until it ends.
    void run(asio::awaitable<int> script) {
        bool               done = false;
        std::exception_ptr failure;
        hosted.loop.spawn(std::move(script), [&](std::exception_ptr error, int) {
            failure = error;
            done    = true;
        });
        const bool finished = hosted.host->run_until([&] { return done; }, 30s);
        if (failure) std::rethrow_exception(failure);
        ASSERT_TRUE(finished) << "the loop ran out of turns before the script ended";
    }

    ProgramVersion admit_javascript(std::string source) {
        ProgramCompiler compiler(fixture.registry, {"program-runtime-test/v1"});
        auto bundle = compiler.compile(ProgramSource::from_javascript("host-pumped.js", std::move(source)));
        return setup_catalog->admit(bundle, ProgramAdmission{kOwner, fixture.profile, fixture.policy, {}});
    }

    ProgramVersion admit(json document) {
        const auto      schema = document["program_schema_version"].get<std::uint32_t>();
        ProgramCompiler compiler(fixture.registry, {"program-runtime-test/v1"});
        auto bundle = compiler.compile(ProgramSource::from_cpp_builder("host-pumped", schema, std::move(document)));
        return setup_catalog->admit(bundle, ProgramAdmission{kOwner, fixture.profile, fixture.policy, {}});
    }

    std::shared_ptr<ProgramCatalog> make_catalog(std::shared_ptr<ProgramStore>       programs,
                                                 std::shared_ptr<const ModuleStore> modules) {
        CatalogConfig config{std::move(programs), fixture.registry, std::make_shared<EngineGenerationCache>(),
                             "program-runtime-test/v1"};
        config.capability_binder = program::detail::single_provider_binder(context, "host-pumped-binding");
        config.module_store      = std::move(modules);
        return std::make_shared<ProgramCatalog>(std::move(config));
    }

    // Setup runs on plain stores, on this thread, before the loop is pumped: this catalog admits every Program.
    Fixture                                    fixture;
    PumpedStores                               stores;
    HostedRuntime                              hosted;
    std::optional<graph::NodeContext>          context       = bound_model();
    std::shared_ptr<InMemoryModuleStore>       inner_modules = std::make_shared<InMemoryModuleStore>();
    std::shared_ptr<MemoryGrantStore>          inner_grants  = std::make_shared<MemoryGrantStore>();
    std::shared_ptr<ProgramCatalog>            setup_catalog = make_catalog(fixture.store.shared(), inner_modules);
    ProgramVersion                             review        = admit(review_document());
    ProgramVersion                             two_pause     = admit(two_pause_document());
    ProgramVersion                             effect        = admit(effect_document());
    ProgramVersion                             tool_only     = admit(tool_only_document());
    ProgramVersion                             child         = admit(child_program_document());
    ProgramVersion                             parent        = admit(expand_task_graph_document(expansion_proposal()));
    std::shared_ptr<ProgramCatalog>            catalog;
    std::shared_ptr<ProgramStore>              wrapped_programs;
    std::shared_ptr<ProgramTransitionStore>    wrapped_transitions;
    std::shared_ptr<TaskGraphFragmentStore>    wrapped_fragments;
    std::shared_ptr<ProgramCoreToolGrantStore> wrapped_grants;
    std::shared_ptr<ModuleStore>               wrapped_modules;
    std::unique_ptr<ProgramRuntime>            runtime;
};


json channel(const ProgramResult& result, const char* name) { return result.output()["channels"][name]["value"]; }

std::string failure_text(const ProgramResult& result) {
    return result.failure() ? result.failure()->code + ": " + result.failure()->message : "no failure";
}

ProgramInvocation plain_invocation(std::string run_id, std::string trace, RunBudget budget = run_budget()) {
    ProgramInvocation invocation{json::object(), std::move(budget), std::move(trace), {}};
    invocation.requested_run_id = std::move(run_id);
    return invocation;
}

std::string pending_call_id(const ProgramResult& result) {
    const auto interrupt = result.interrupt();
    if (!interrupt) throw std::invalid_argument("the result is not interrupted");
    if (interrupt->pending_input) return interrupt->pending_input->call_id();
    return interrupt->pending_effect->call_id();
}

ProgramResume answer_for(const ProgramResult& paused, json value, std::string trace) {
    return ProgramResume{std::move(value), std::move(trace), {}, pending_call_id(paused)};
}

RunInvocation fork_invocation(const ProgramVersion& version, const ProgramResult& source, std::string run_id) {
    auto invocation          = top_level(version.id(), std::move(run_id));
    invocation.budget        = neograph::test::fork_share(source.remaining_budget());
    invocation.message_sequence = 19;
    invocation.idempotency_key  = "host-pumped-fork:" + invocation.run_id;
    invocation.validate();
    return invocation;
}

// The harness every test below uses: the whole script is one coroutine of the host loop.
// Every store call was completed by the host's thread.
#define EXPECT_COMPLETED_ON_THE_HOST_THREAD(host_)                                                           \
    EXPECT_EQ((host_).stores.transitions->off_thread() + (host_).stores.programs->off_thread() +            \
                  (host_).stores.grants->off_thread() + (host_).stores.fragments->off_thread() +            \
                  (host_).stores.modules->off_thread(),                                                      \
              0U)                                                                                            \
        << "a store call was completed by a thread other than the host's"
// The runtime started no thread. A tool the Core dispatches runs on the process's blocking pool, which starts
// threads on first use, so the tests that run a tool do not check this.
#define EXPECT_NO_THREAD_STARTED(census_)                                                               \
    do {                                                                                                 \
        if ((census_).available()) {                                                                     \
            const auto started_ = (census_).started();                                                   \
            EXPECT_TRUE(started_.empty()) << "the runtime started " << started_.size() << " thread(s)"; \
        }                                                                                                \
    } while (false)

}  // namespace

// ---- The census the tests below rely on ----

TEST(ThreadCensus, ReportsAThreadStartedAfterItAndNotOneThatEnded) {
    std::atomic<bool> stop{false};
    std::thread       earlier([&] {
        while (!stop) std::this_thread::yield();
    });
    const neograph::test::ThreadCensus census;
    if (!census.available()) {
        stop = true;
        earlier.join();
        GTEST_SKIP() << "the platform cannot list threads";
    }
    EXPECT_TRUE(census.started().empty());
    // A thread that was listed and has since ended is not a start.
    stop = true;
    earlier.join();
    EXPECT_TRUE(census.started().empty());
    // A thread that appears after the census is one.
    std::atomic<bool> release{false};
    std::thread       later([&] {
        while (!release) std::this_thread::yield();
    });
    EXPECT_EQ(census.started().size(), 1U);
    release = true;
    later.join();
}

// ---- One path at a time ----

TEST(ProgramHostPumpedStores, StartingARunReadsTheCatalogOverAHostPumpedProgramStore) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Programs);
    host.stores.programs->set_pumped(true);
    const neograph::test::ThreadCensus census;

    std::optional<ProgramResult> result;
    auto script = [&]() -> asio::awaitable<int> {
        auto handle = co_await host.runtime->start_async(kOwner, host.child, plain_invocation("child-1", "trace-catalog", child_budget()));
        result      = co_await handle.wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed) << failure_text(*result);
    EXPECT_EQ(child_runs.load(), 1);
    EXPECT_GT(host.stores.programs->suspended(), 0U) << "the run never waited for a turn of the host";
    EXPECT_TRUE(host.stores.programs->operation("program.get_version") > 0 ||
                host.stores.programs->operation("program.get_bundle") > 0);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
    EXPECT_NO_THREAD_STARTED(census);
}

TEST(ProgramHostPumpedStores, ATerminalStateIsPublishedThroughAHostPumpedTransitionStore) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    const neograph::test::ThreadCensus census;
    // The run starts over plain stores. Its node switches the transition store to host turns, so that everything the
    // run does after it, the publication of its terminal state included, waits for the loop.
    on_child_node = [&] { host.stores.transitions->set_pumped(true); };

    std::optional<ProgramResult> result;
    auto script = [&]() -> asio::awaitable<int> {
        auto handle = co_await host.runtime->start_async(kOwner, host.child, plain_invocation("child-2", "trace-terminal", child_budget()));
        result      = co_await handle.wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed) << failure_text(*result);
    EXPECT_GT(host.stores.transitions->operation("transition.compare_publish") +
                  host.stores.transitions->operation("transition.compare_publish_execution"),
              0U);
    EXPECT_GT(host.stores.transitions->suspended(), 0U);
    // The record the host reads back is the one that was published.
    const auto stored = host.fixture.journal->load(kOwner, "child-2");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->continuation().state, ContinuationState::Completed);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
    EXPECT_NO_THREAD_STARTED(census);
}

TEST(ProgramHostPumpedStores, ACoreToolGrantIsReadFromAHostPumpedGrantStore) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Grants);
    host.grant_tool("tool-granted", host.tool_only);
    host.stores.grants->set_pumped(true);

    std::optional<ProgramResult> granted, ungranted;
    auto script = [&]() -> asio::awaitable<int> {
        auto first = co_await host.runtime->start_async(kOwner, host.tool_only, plain_invocation("tool-granted", "trace-granted"));
        granted    = co_await first.wait_async();
        auto second = co_await host.runtime->start_async(kOwner, host.tool_only, plain_invocation("tool-ungranted", "trace-ungranted"));
        ungranted   = co_await second.wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(granted && ungranted);
    EXPECT_EQ(granted->status(), ProgramTerminalStatus::Completed) << failure_text(*granted);
    EXPECT_EQ(ungranted->status(), ProgramTerminalStatus::Completed) << failure_text(*ungranted);
    // Only the run the host admitted a grant for ran the tool.
    EXPECT_EQ(tool_runs.load(), 1);
    EXPECT_GT(host.stores.grants->suspended(), 0U);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

TEST(ProgramHostPumpedStores, AChildIsStartedAndItsCompletionPublishedOverHostPumpedStores) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions | HostPumpedProgramHost::Fragments);
    const neograph::test::ThreadCensus census;

    std::optional<ProgramResult> result;
    auto script = [&]() -> asio::awaitable<int> {
        ProgramInvocation invocation{json{{"item", "seed"}}, RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                                     "trace-parent", {}};
        invocation.requested_run_id = "parent-1";
        auto handle = co_await host.runtime->start_async(kOwner, host.parent, std::move(invocation));
        // The parent is running; from here on every store takes turns of the loop.
        host.stores.pump_all();
        result = co_await handle.wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), ProgramTerminalStatus::Completed) << failure_text(*result);
    EXPECT_EQ(child_runs.load(), 2);
    const auto output = result->output();
    ASSERT_TRUE(output.contains("tasks"));
    ASSERT_EQ(output["tasks"].size(), 2U);
    for (const auto& task : output["tasks"]) EXPECT_EQ(task["state"], "completed");
    EXPECT_GT(host.stores.transitions->suspended(), 0U);
    EXPECT_GT(host.stores.fragments->suspended(), 0U);
    // The parent's record holds both children as completed: their completions were published into it.
    const auto record = host.fixture.journal->load(kOwner, "parent-1");
    ASSERT_TRUE(record);
    ASSERT_EQ(record->children().size(), 2U);
    for (const auto& child_record : record->children()) EXPECT_EQ(child_record.state, ProgramChildState::Completed);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
    EXPECT_NO_THREAD_STARTED(census);
}

TEST(ProgramHostPumpedStores, AResumeAndAForkAwaitTheirStores) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("review-resumed", host.review);
    host.grant_tool("review-forked", host.review);

    std::optional<ProgramResult> paused, resumed, forked;
    auto script = [&]() -> asio::awaitable<int> {
        auto first = co_await host.runtime->start_async(top_level(host.review.id(), "review-resumed"));
        paused     = co_await first.wait_async();
        // The run waits for its answer over plain stores. Answering it, and forking it, take turns of the loop.
        host.stores.pump_all();
        auto answer         = answer_for(*paused, json{{"decision", "approved"}}, "trace-resume");
        auto resumed_handle = co_await host.runtime->resume_handle_async(kOwner, "review-resumed", std::move(answer));
        resumed             = co_await resumed_handle.wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(paused && resumed);
    ASSERT_EQ(paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*paused);
    EXPECT_EQ(resumed->status(), ProgramTerminalStatus::Completed) << failure_text(*resumed);
    EXPECT_EQ(channel(*resumed, "value"), "approved");
    EXPECT_EQ(model_calls.load(), 1);
    EXPECT_EQ(tool_runs.load(), 1);

    // A fork of a second paused run: the paused run is untouched, the fork answers for itself.
    host.stores.pump_all(false);
    std::optional<ProgramResult> source;
    auto fork_script = [&]() -> asio::awaitable<int> {
        auto second = co_await host.runtime->start_async(top_level(host.review.id(), "review-source"));
        source      = co_await second.wait_async();
        host.stores.pump_all();
        ExactProgramCheckpointReference reference{source->run_id(), source->checkpoint()->checkpoint_id};
        auto invocation = fork_invocation(host.review, *source, "review-forked");
        auto answer     = answer_for(*source, json{{"decision", "forked"}}, "trace-fork");
        auto handle     = co_await host.runtime->fork_async(std::move(reference), std::move(invocation), std::move(answer));
        forked          = co_await handle.wait_async();
        co_return 0;
    };
    host.run(fork_script());
    ASSERT_TRUE(source && forked);
    ASSERT_EQ(source->status(), ProgramTerminalStatus::Interrupted) << failure_text(*source);
    EXPECT_EQ(forked->status(), ProgramTerminalStatus::Completed) << failure_text(*forked);
    EXPECT_EQ(channel(*forked, "value"), "forked");
    EXPECT_EQ(model_calls.load(), 2);
    EXPECT_GT(host.stores.transitions->suspended(), 0U);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

namespace {

struct ForkAttempt {
    std::optional<ProgramResult> result;
    std::string                  refusal;  // the code of the runtime's refusal, or the text of another failure
    bool                         done = false;
};

// One fork of `source`, as a task of the host's loop; its outcome lands in `attempt`.
asio::awaitable<int> fork_once(ProgramRuntime& runtime, const ProgramResult& source, RunInvocation invocation,
                               ForkAttempt& attempt) {
    ExactProgramCheckpointReference reference{source.run_id(), source.checkpoint()->checkpoint_id};
    auto                            answer = answer_for(source, json{{"decision", "forked"}}, "trace-fork-" + source.run_id());
    try {
        auto handle    = co_await runtime.fork_async(std::move(reference), std::move(invocation), std::move(answer));
        attempt.result = co_await handle.wait_async();
    } catch (const ProgramDiagnosticError& error) {
        attempt.refusal = error.diagnostic().code;
    } catch (const std::exception& error) {
        attempt.refusal = std::string("exception: ") + error.what();
    }
    attempt.done = true;
    co_return 0;
}

RunInvocation shared_fork_invocation(const ProgramVersion& version, std::string run_id, const RunBudget& budget) {
    auto invocation             = top_level(version.id(), std::move(run_id));
    invocation.budget           = budget;
    invocation.message_sequence = 19;
    invocation.idempotency_key  = "host-pumped-fork:" + invocation.run_id;
    invocation.validate();
    return invocation;
}

// The Program of the tests below: it asks once and ends with the answer. It admits enough operations to be shared.
ProgramVersion shareable_program(HostPumpedProgramHost& host) {
    auto document = program_document("host-approve");
    document["program_schema_version"]                     = PROGRAM_SCHEMA_VERSION_V2;
    document["declared_budget_requirements"][4]["maximum"] = 16;
    return host.admit(std::move(document));
}

// Starts a run of `version` and waits until it is paused, over the host's turns.
ProgramResult pause_a_run(HostPumpedProgramHost& host, const ProgramVersion& version, const std::string& run_id) {
    std::optional<ProgramResult> paused;
    auto script = [&]() -> asio::awaitable<int> {
        auto invocation                           = top_level(version.id(), run_id);
        invocation.budget.max_program_operations = 16;
        auto handle                               = co_await host.runtime->start_async(std::move(invocation));
        paused                                    = co_await handle.wait_async();
        co_return 0;
    };
    host.run(script());
    EXPECT_TRUE(paused && paused->status() == ProgramTerminalStatus::Interrupted);
    return *paused;
}

// Makes `count` forks of `source` in one turn of the loop, each asking for `each`, and pumps the loop until every one has
// ended.
std::vector<ForkAttempt> fork_in_one_turn(HostPumpedProgramHost& host, const ProgramVersion& version, const ProgramResult& source,
                                          int count, const RunBudget& each) {
    host.stores.pump_all();
    std::vector<ForkAttempt> attempts(static_cast<std::size_t>(count));
    // No turn of the loop runs between these spawns: the forks start together, as a host's microtasks do.
    for (int i = 0; i < count; ++i) {
        auto& attempt = attempts[static_cast<std::size_t>(i)];
        host.hosted.loop.spawn(
            fork_once(*host.runtime, source, shared_fork_invocation(version, "same-turn-fork-" + std::to_string(i), each), attempt),
            [](std::exception_ptr, int) {});
    }
    const bool finished = host.hosted.host->run_until(
        [&] { return std::all_of(attempts.begin(), attempts.end(), [](const ForkAttempt& a) { return a.done; }); }, 30s);
    EXPECT_TRUE(finished) << "the loop ran out of turns before every fork ended";
    host.stores.pump_all(false);
    return attempts;
}

}  // namespace

// The forks of one pause that a host makes in the same turn of its loop cannot wait for one another, and the stores
// answer in later turns. Each is published against the lineage the one before it left, so the budgets they take add up to
// at most what the paused run had, and the paused run keeps the rest.
TEST(ProgramHostPumpedStores, ForksOfOnePauseMadeInTheSameTurnAreServedOneAfterAnother) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    const auto            version = shareable_program(host);
    const auto            source  = pause_a_run(host, version, "pause-shared");
    const auto            left    = host.fixture.journal->load_run_lineage(kOwner, "pause-shared")->remaining_budget();

    // Four forks of a fifth of what is left: all fit.
    const auto each_fifth = neograph::test::fork_fraction(left, 1, 5);
    const auto fits       = fork_in_one_turn(host, version, source, 4, each_fifth);
    RunBudget  taken{};
    for (std::size_t i = 0; i < fits.size(); ++i) {
        ASSERT_TRUE(fits[i].result) << "fork " << i << ": " << fits[i].refusal;
        EXPECT_EQ(fits[i].result->status(), ProgramTerminalStatus::Completed) << failure_text(*fits[i].result);
        const auto record = host.fixture.journal->load(kOwner, "same-turn-fork-" + std::to_string(i));
        ASSERT_TRUE(record);
        // The record holds the amounts the fork was given, whatever the forks around it did.
        EXPECT_EQ(record->invocation().budget, each_fifth);
        taken.wall_time_ms += record->invocation().budget.wall_time_ms;
        taken.model_tokens += record->invocation().budget.model_tokens;
        taken.max_program_operations += record->invocation().budget.max_program_operations;
        taken.max_core_steps += record->invocation().budget.max_core_steps;
    }
    const auto after = host.fixture.journal->load_run_lineage(kOwner, "pause-shared");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->remaining_budget().wall_time_ms, left.wall_time_ms - taken.wall_time_ms);
    EXPECT_EQ(after->remaining_budget().model_tokens, left.model_tokens - taken.model_tokens);
    EXPECT_EQ(after->remaining_budget().max_program_operations, left.max_program_operations - taken.max_program_operations);
    EXPECT_EQ(after->remaining_budget().max_core_steps, left.max_core_steps - taken.max_core_steps);

    // The same forks again, in one turn, are the forks that were made: nothing is debited a second time.
    const auto again = fork_in_one_turn(host, version, source, 4, each_fifth);
    for (std::size_t i = 0; i < again.size(); ++i) {
        ASSERT_TRUE(again[i].result) << "replayed fork " << i << ": " << again[i].refusal;
        EXPECT_EQ(again[i].result->run_id(), fits[i].result->run_id());
    }
    const auto replayed = host.fixture.journal->load_run_lineage(kOwner, "pause-shared");
    ASSERT_TRUE(replayed);
    EXPECT_EQ(replayed->remaining_budget(), after->remaining_budget());
}

TEST(ProgramHostPumpedStores, ForksMadeInTheSameTurnThatAskForMoreThanIsLeftAreRefusedForTheBudget) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    const auto            version = shareable_program(host);
    const auto            source  = pause_a_run(host, version, "pause-oversubscribed");
    const auto            left    = host.fixture.journal->load_run_lineage(kOwner, "pause-oversubscribed")->remaining_budget();

    // Four forks of two fifths: two fit, and the others ask for more than the first two left.
    const auto each     = neograph::test::fork_fraction(left, 2, 5);
    const auto attempts = fork_in_one_turn(host, version, source, 4, each);
    int        completed = 0, refused = 0;
    for (std::size_t i = 0; i < attempts.size(); ++i) {
        if (attempts[i].result) {
            ++completed;
            EXPECT_EQ(attempts[i].result->status(), ProgramTerminalStatus::Completed) << failure_text(*attempts[i].result);
        } else {
            ++refused;
            EXPECT_EQ(attempts[i].refusal, "P_FORK_BUDGET") << "fork " << i;
            EXPECT_FALSE(host.fixture.journal->load(kOwner, "same-turn-fork-" + std::to_string(i)));
        }
    }
    EXPECT_EQ(completed, 2);
    EXPECT_EQ(refused, 2);
    const auto after = host.fixture.journal->load_run_lineage(kOwner, "pause-oversubscribed");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->remaining_budget().model_tokens, left.model_tokens - 2 * each.model_tokens);
    EXPECT_EQ(after->remaining_budget().wall_time_ms, left.wall_time_ms - 2 * each.wall_time_ms);
    EXPECT_EQ(after->remaining_budget().max_program_operations, left.max_program_operations - 2 * each.max_program_operations);
    EXPECT_EQ(after->remaining_budget().max_core_steps, left.max_core_steps - 2 * each.max_core_steps);
}

namespace {

// Sets a flag when its first event arrives, and nothing else.
class FirstEventSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent&) override { seen_first = true; }
    bool seen_first = false;
};

}  // namespace

// A host that asks to cancel a run when its first event arrives can only do so once the handle exists: the handle of a
// resume is returned after the first event was delivered (#557). The cancellation then ends the run Cancelled.
TEST(ProgramHostPumpedStores, AResumedRunCancelledAtItsFirstEventEndsCancelled) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("review-resume-cancel", host.review);

    std::optional<ProgramResult>    paused, ended;
    std::optional<ProgramRunRecord> stored;
    bool                            applied = false;
    auto                            sink    = std::make_shared<FirstEventSink>();
    bool                            first_event_before_handle = false;
    auto script = [&]() -> asio::awaitable<int> {
        auto first = co_await host.runtime->start_async(top_level(host.review.id(), "review-resume-cancel"));
        paused     = co_await first.wait_async();
        host.stores.pump_all();
        auto answer = answer_for(*paused, json{{"decision", "approved"}}, "trace-resume-cancel");
        answer.events = sink;
        auto resumed_handle =
            co_await host.runtime->resume_handle_async(kOwner, "review-resume-cancel", std::move(answer));
        first_event_before_handle = sink->seen_first;
        // Let the attempt of the resumed run start before the cancellation arrives, as a host's own callback does.
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        applied                   = co_await resumed_handle.cancel_async();
        ended                     = co_await resumed_handle.wait_async();
        stored                    = co_await resumed_handle.snapshot_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(paused && ended && stored);
    ASSERT_EQ(paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*paused);
    EXPECT_TRUE(first_event_before_handle) << "the first event of a resume arrives before its handle does";
    EXPECT_TRUE(applied);
    EXPECT_EQ(ended->status(), ProgramTerminalStatus::Cancelled) << failure_text(*ended);
    ASSERT_TRUE(stored->terminal_result());
    EXPECT_EQ(stored->terminal_result()->status(), ProgramTerminalStatus::Cancelled);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

namespace {

// A Program with a JavaScript main() that calls one graph, whose node asks until it is resumed.
const char* const kJavaScriptApproval = R"JS(
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
  graph.node("approve", {type: "host-approve"});
  graph.entry("approve");
  graph.exit("approve");
  return graph;
}

export function* main(input) {
  const output = yield ng.callCore("main", {}, "resume:host-pumped");
  return {value: output.value};
}
)JS";

// Acts on the first event of a kind: by default of the Core call that a resume continues, when no node of the call has
// run yet.
class FirstCoreEventSink final : public ProgramEventSink {
public:
    void on_event(const ProgramEvent& event) override {
        if (event.kind != watched || acted) return;
        acted = true;
        if (action) action();
    }
    ProgramEventKind      watched = ProgramEventKind::Core;
    std::function<void()> action;  // may throw, as a host callback that rejects the event does
    bool                  acted = false;
};

enum class EndOfAResumedRun {
    CancelBeforeTheCallStarts,
    HandlerThrowsAtTheStartedEvent,
    CancelAtTheStartOfTheCall,
    HandlerThrowsAtTheStartOfTheCall,
    // The checkpoint of the resume is gone when its attempt reads it.
    CheckpointGoneAtTheStartedEvent,
    // The attempt waits for host capacity and is cancelled; the host refuses it; the host's resolver throws.
    CancelledWhileWaitingForHostCapacity,
    RefusedByHostAdmission,
    HostAdmissionResolverThrows
};

struct ResumedJavaScriptEnd {
    std::optional<ProgramResult>        paused, ended;
    std::optional<ProgramJournalRecord> held, latest;
    std::optional<ProgramRunRecord>     record;
    std::optional<ProgramResult>        found;
    bool                                applied = false;
};

// Resumes a paused run of the JavaScript Program and ends it before the first node of the resumed call runs: before the
// call has started, or at its first event.
ResumedJavaScriptEnd end_a_resumed_javascript_run(EndOfAResumedRun how, const std::string& run_id) {
    auto       checkpoints = std::make_shared<neograph::test::VanishingCheckpointStore>();
    const auto host_profile = neograph::HostResourceProfile::create(neograph::HostResourceProfileData{
        "host-pumped-resumed-v1",
        neograph::HostResourceVector{.cpu_millis = 1},
        {},
        neograph::HostResourceEvidence{"test", neograph::HostResourceConfidence::Measured, 1, false}});
    auto       controller = std::make_shared<neograph::HostAdmissionController>(
        neograph::HostAdmissionControllerConfig{host_profile});
    const bool at_host_admission = how == EndOfAResumedRun::CancelledWhileWaitingForHostCapacity ||
                                   how == EndOfAResumedRun::RefusedByHostAdmission ||
                                   how == EndOfAResumedRun::HostAdmissionResolverThrows;
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions, true, checkpoints, [&](RuntimeConfig& config) {
        if (!at_host_admission) return;
        config.host_admission          = controller;
        config.host_admission_resolver = [how](const ProgramHostAdmissionContext& context) {
            neograph::HostAdmissionRequest request;
            request.resources.cpu_millis = 1;
            if (context.attempt < 2) return request;
            if (how == EndOfAResumedRun::RefusedByHostAdmission) request.resources.cpu_millis = 1000;
            if (how == EndOfAResumedRun::HostAdmissionResolverThrows)
                throw std::runtime_error("the resolver failed");
            return request;
        };
    });
    const auto            version = host.admit_javascript(kJavaScriptApproval);
    ResumedJavaScriptEnd  end;
    auto                  sink = std::make_shared<FirstCoreEventSink>();
    auto script = [&]() -> asio::awaitable<int> {
        auto invocation   = top_level(version.id(), run_id);
        invocation.budget = RunBudget{10000, 1000, 1000, 1, 8, 20, 0, 0, 0};
        invocation.validate();
        auto first        = co_await host.runtime->start_async(std::move(invocation));
        end.paused        = co_await first.wait_async();
        host.stores.pump_all();
        auto answer   = answer_for(*end.paused, json{{"decision", "approved"}}, "trace-js-" + run_id);
        answer.events = sink;
        if (how == EndOfAResumedRun::HandlerThrowsAtTheStartedEvent) {
            // The resume is rejected on its first event, once it is published, before its handle exists.
            sink->watched = ProgramEventKind::Started;
            sink->action  = [&] {
                end.held = host.fixture.journal->latest(kOwner, run_id);
                throw std::runtime_error("the handler rejected the resume");
            };
        }
        if (how == EndOfAResumedRun::CheckpointGoneAtTheStartedEvent) {
            // The checkpoint is gone from the first event of the resume on, before its attempt exists.
            sink->watched = ProgramEventKind::Started;
            sink->action  = [&] {
                end.held = host.fixture.journal->latest(kOwner, run_id);
                checkpoints->vanish();
            };
        }
        // The capacity of the host is taken, so that the attempt of the resume has to wait for it.
        std::optional<neograph::HostResourceLease> capacity;
        if (how == EndOfAResumedRun::CancelledWhileWaitingForHostCapacity) {
            neograph::HostAdmissionRequest request;
            request.resources.cpu_millis = 1;
            capacity                      = controller->try_reserve(std::move(request));
            EXPECT_TRUE(capacity.has_value()) << "the first attempt gave its capacity back";
        }
        auto handle = co_await host.runtime->resume_handle_async(kOwner, run_id, std::move(answer));
        // The resume is published and its attempt is not running yet: the reservation of the call is held.
        if (how != EndOfAResumedRun::HandlerThrowsAtTheStartedEvent &&
            how != EndOfAResumedRun::CheckpointGoneAtTheStartedEvent)
            end.held = host.fixture.journal->latest(kOwner, run_id);
        switch (how) {
            case EndOfAResumedRun::CancelBeforeTheCallStarts:
            case EndOfAResumedRun::CancelledWhileWaitingForHostCapacity:
                end.applied = co_await handle.cancel_async();
                break;
            case EndOfAResumedRun::HandlerThrowsAtTheStartedEvent:
            case EndOfAResumedRun::CheckpointGoneAtTheStartedEvent:
            case EndOfAResumedRun::RefusedByHostAdmission:
            case EndOfAResumedRun::HostAdmissionResolverThrows:
                break;
            case EndOfAResumedRun::CancelAtTheStartOfTheCall:
                sink->action = [&] { end.applied = handle.cancel(); };
                break;
            case EndOfAResumedRun::HandlerThrowsAtTheStartOfTheCall:
                sink->action = [] { throw std::runtime_error("the handler rejected the first Core event"); };
                break;
        }
        end.ended  = co_await handle.wait_async();
        end.record = co_await handle.snapshot_async();
        co_return 0;
    };
    host.run(script());
    end.latest = host.fixture.journal->latest(kOwner, run_id);
    // Host turns for the stores are over: a fresh read of the run, as a reconnect makes it.
    host.stores.pump_all(false);
    auto reconnect_script = [&]() -> asio::awaitable<int> {
        auto handle = co_await host.runtime->reconnect_async(kOwner, run_id);
        end.found   = co_await handle.wait_async();
        co_return 0;
    };
    host.run(reconnect_script());
    EXPECT_EQ(approvals_run.load(), 1) << "no node ran after the resume";
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
    return end;
}

// Before the Core call of a resumed run has started, nothing settles the reservation of the call: the run ends in the
// state it was asked to with the remaining budget the resume left it, and gives none of the reservation back. Once the
// call has started, it settles the reservation with the usage it measured, and the part it did not use comes back.
void expect_the_end_of_a_resumed_javascript_run(const ResumedJavaScriptEnd& end, ProgramTerminalStatus status,
                                                ContinuationState state, bool call_started) {
    ASSERT_TRUE(end.paused && end.ended && end.held && end.latest && end.record && end.found);
    ASSERT_EQ(end.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*end.paused);
    EXPECT_NE(end.held->inflight_reservation, RunBudget{}) << "the resume holds the reservation of the call";
    EXPECT_EQ(end.ended->status(), status) << failure_text(*end.ended);
    EXPECT_EQ(end.latest->continuation.state, state);
    EXPECT_EQ(end.latest->inflight_reservation, RunBudget{});
    if (call_started) {
        EXPECT_EQ(end.latest->sequence, end.held->sequence + 2) << "the call settled its reservation, then the run ended";
        EXPECT_GT(end.latest->remaining_budget.model_tokens, end.held->remaining_budget.model_tokens)
            << "the unused part of the reservation came back";
    } else {
        EXPECT_EQ(end.latest->sequence, end.held->sequence + 1) << "the ending is the one publication after the resume";
        EXPECT_EQ(end.latest->remaining_budget, end.held->remaining_budget) << "nothing of the reservation was returned";
        EXPECT_EQ(end.ended->remaining_budget(), end.held->remaining_budget);
    }
    ASSERT_TRUE(end.record->terminal_result());
    EXPECT_EQ(end.record->terminal_result()->status(), status);
    EXPECT_EQ(end.record->continuation().state, state);
    EXPECT_EQ(end.found->status(), status) << failure_text(*end.found);
}

}  // namespace

// The first event of a resume, Started, is delivered once the resume is published and before its attempt exists. When
// the handler throws on it, no node of the resumed call has run: the run ends Failed with the code of the handler's
// error and the remaining budget the resume left it, and none of the reservation of the call comes back.
TEST(ProgramHostPumpedStores, AResumedJavaScriptRunWhoseHandlerThrowsAtItsStartedEventEndsFailedWithTheHandlersCode) {
    EndOfAResumedRun how = EndOfAResumedRun::HandlerThrowsAtTheStartedEvent;
    const auto       end = end_a_resumed_javascript_run(how, "js-throws-at-started");
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Failed, ContinuationState::Failed, false);
    ASSERT_TRUE(end.ended && end.ended->failure());
    EXPECT_EQ(end.ended->failure()->code, "P_EVENT_SINK");
}

// A run with a JavaScript main() holds the reservation of the Core call it resumes. One that is cancelled before the
// call has started ends terminal on the host's loop too, and returns none of the reservation (#557).
TEST(ProgramHostPumpedStores, AResumedJavaScriptRunCancelledBeforeItsCoreCallStartsEndsCancelled) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::CancelBeforeTheCallStarts, "js-cancel-right-away");
    EXPECT_TRUE(end.applied);
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Cancelled, ContinuationState::Cancelled, false);
}

// The two that follow end a run once its call has started: they were never affected, and stay as they were.
TEST(ProgramHostPumpedStores, AResumedJavaScriptRunCancelledAtTheStartOfItsCoreCallSettlesTheReservation) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::CancelAtTheStartOfTheCall, "js-cancel-at-start");
    EXPECT_TRUE(end.applied);
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Cancelled, ContinuationState::Cancelled, true);
}

TEST(ProgramHostPumpedStores, AResumedJavaScriptRunWhoseHandlerThrowsAtTheStartOfItsCoreCallEndsFailedWithTheHandlersCode) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::HandlerThrowsAtTheStartOfTheCall, "js-handler-throws");
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Failed, ContinuationState::Failed, true);
    ASSERT_TRUE(end.ended && end.ended->failure());
    EXPECT_EQ(end.ended->failure()->code, "P_EVENT_SINK");
}

// The checkpoint a resume continues from can be gone when its attempt reads it (a restarted process over stores that do not
// agree): the run ends CheckpointIncompatible before its call has started, and returns none of the reservation.
TEST(ProgramHostPumpedStores, AResumedJavaScriptRunWhoseCheckpointIsGoneEndsCheckpointIncompatible) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::CheckpointGoneAtTheStartedEvent, "js-checkpoint-gone");
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::CheckpointIncompatible,
                                               ContinuationState::CheckpointIncompatible, false);
    ASSERT_TRUE(end.ended && end.ended->failure());
    EXPECT_EQ(end.ended->failure()->code, "P_CHECKPOINT_INCOMPATIBLE");
}

// The attempt of a resume that waits for host capacity has started nothing and holds the reservation of its call: the
// cancellation, the host's refusal and a throwing resolver each end the run terminal with the remaining budget the resume
// left it.
TEST(ProgramHostPumpedStores, AResumedJavaScriptRunCancelledWhileItWaitsForHostCapacityEndsCancelled) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::CancelledWhileWaitingForHostCapacity, "js-host-cancel");
    EXPECT_TRUE(end.applied);
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Cancelled, ContinuationState::Cancelled, false);
}

TEST(ProgramHostPumpedStores, AResumedJavaScriptRunRefusedByHostAdmissionEndsFailedWithItsCode) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::RefusedByHostAdmission, "js-host-refused");
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Failed, ContinuationState::Failed, false);
    ASSERT_TRUE(end.ended && end.ended->failure());
    EXPECT_EQ(end.ended->failure()->code, "P_HOST_ADMISSION");
}

TEST(ProgramHostPumpedStores, AResumedJavaScriptRunWhoseHostAdmissionResolverThrowsEndsFailed) {
    const auto end = end_a_resumed_javascript_run(EndOfAResumedRun::HostAdmissionResolverThrows, "js-host-resolver-throws");
    expect_the_end_of_a_resumed_javascript_run(end, ProgramTerminalStatus::Failed, ContinuationState::Failed, false);
    ASSERT_TRUE(end.ended && end.ended->failure());
    EXPECT_EQ(end.ended->failure()->code, "P_RUNTIME_CORE_FAILURE");
}

namespace {

// A Program with a JavaScript main() that calls one graph, whose node leaves an effect to reconcile.
const char* const kJavaScriptEffect = R"JS(
export function define() {
  const graph = ng.graph("main");
  graph.channel("value", {reducer: "runtime-overwrite", initial: ""});
  graph.node("effect", {type: "host-effect"});
  graph.entry("effect");
  graph.exit("effect");
  return graph;
}

export function* main(input) {
  const output = yield ng.callCore("main", {}, "reconcile:host-pumped");
  return {value: output.value};
}
)JS";

ProgramEffectResolution resolution_for(std::string call_id, ProgramEffectReconciliation how, std::optional<json> result,
                                       std::string trace, std::shared_ptr<ProgramEventSink> events) {
    return ProgramEffectResolution{std::move(call_id), how, std::move(result), std::move(trace), std::move(events)};
}

}  // namespace

// The first event of a reconciliation, Started, is delivered once it is published and before its attempt exists. When the
// handler throws on it, no node of the continued call has run: the run ends Failed with the code of the handler's error
// and the remaining budget the reconciliation left it, on the host's loop as on threads.
TEST(ProgramHostPumpedStores, AReconciledJavaScriptCallCoreWhoseHandlerThrowsAtItsStartedEventEndsFailedWithTheHandlersCode) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions, true);
    const auto            version = host.admit_javascript(kJavaScriptEffect);
    const std::string     run_id  = "js-reconcile-throws";

    std::optional<ProgramResult>        paused, ambiguous, ended, found;
    std::optional<ProgramJournalRecord> held, latest;
    std::optional<ProgramRunRecord>     record;
    auto                                sink = std::make_shared<FirstCoreEventSink>();
    sink->watched = ProgramEventKind::Started;
    sink->action  = [&] {
        held = host.fixture.journal->latest(kOwner, run_id);
        throw std::runtime_error("the handler rejected the reconciliation");
    };
    auto script = [&]() -> asio::awaitable<int> {
        auto invocation   = top_level(version.id(), run_id);
        invocation.budget = RunBudget{10000, 1000, 1000, 1, 8, 20, 0, 0, 0};
        invocation.validate();
        auto first = co_await host.runtime->start_async(std::move(invocation));
        paused     = co_await first.wait_async();
        host.stores.pump_all();
        const auto call_id = pending_call_id(*paused);
        auto       unknown_resolution   = resolution_for(call_id, ProgramEffectReconciliation::Unknown, std::nullopt, "trace-unknown", nullptr);
        auto       completed_resolution = resolution_for(call_id, ProgramEffectReconciliation::Completed, json{{"result", "reconciled"}},
                                                         "trace-completed", sink);
        auto       unknown = co_await host.runtime->reconcile_async(kOwner, run_id, std::move(unknown_resolution));
        ambiguous = co_await unknown.wait_async();
        auto handle = co_await host.runtime->reconcile_async(kOwner, run_id, std::move(completed_resolution));
        ended  = co_await handle.wait_async();
        record = co_await handle.snapshot_async();
        co_return 0;
    };
    host.run(script());
    latest = host.fixture.journal->latest(kOwner, run_id);
    host.stores.pump_all(false);
    auto reconnect_script = [&]() -> asio::awaitable<int> {
        auto handle = co_await host.runtime->reconnect_async(kOwner, run_id);
        found       = co_await handle.wait_async();
        co_return 0;
    };
    host.run(reconnect_script());

    ASSERT_TRUE(paused && ambiguous && ended && record && held && latest && found);
    ASSERT_EQ(paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*paused);
    ASSERT_EQ(ambiguous->status(), ProgramTerminalStatus::AmbiguousEffect) << failure_text(*ambiguous);
    EXPECT_EQ(held->continuation.state, ContinuationState::Running);
    EXPECT_EQ(ended->status(), ProgramTerminalStatus::Failed) << failure_text(*ended);
    ASSERT_TRUE(ended->failure());
    EXPECT_EQ(ended->failure()->code, "P_EVENT_SINK");
    EXPECT_EQ(latest->continuation.state, ContinuationState::Failed);
    EXPECT_EQ(latest->sequence, held->sequence + 1);
    EXPECT_EQ(latest->remaining_budget, held->remaining_budget);
    EXPECT_EQ(latest->inflight_reservation, RunBudget{});
    ASSERT_TRUE(record->terminal_result());
    EXPECT_EQ(record->continuation().state, ContinuationState::Failed);
    EXPECT_EQ(found->status(), ProgramTerminalStatus::Failed) << failure_text(*found);
    EXPECT_EQ(approvals_run.load(), 1) << "no node ran after the reconciliation";
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

TEST(ProgramHostPumpedStores, AnInterruptedRunIsCancelledThroughAHostPumpedStore) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    std::optional<ProgramResult> paused, cancelled;
    bool                         applied = false;
    bool                         again   = true;
    auto script = [&]() -> asio::awaitable<int> {
        auto handle = co_await host.runtime->start_async(top_level(host.review.id(), "review-cancelled"));
        paused      = co_await handle.wait_async();
        // The run waits for its answer. Cancelling it publishes its Cancelled state, which takes turns of the loop.
        host.stores.pump_all();
        applied   = co_await handle.cancel_async();
        cancelled = co_await handle.wait_async();
        again     = co_await handle.cancel_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(paused && cancelled);
    ASSERT_EQ(paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*paused);
    EXPECT_TRUE(applied);
    EXPECT_FALSE(again) << "a run that is cancelled already is not cancelled twice";
    EXPECT_EQ(cancelled->status(), ProgramTerminalStatus::Cancelled);
    const auto stored = host.fixture.journal->load(kOwner, "review-cancelled");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->continuation().state, ContinuationState::Cancelled);
    EXPECT_GT(host.stores.transitions->suspended(), 0U);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

// ---- The synchronous cancel() of an interrupted run ----
//
// A host-pumped executor cannot be waited on, so ProgramHandle::cancel() does not write the Cancelled state of an
// interrupted run itself. It accepts the cancellation, and a task of the loop writes the state: the call is true when
// it stopped a live run or scheduled the cancellation of an interrupted one, and false when the run is over, or
// cancelled, or its cancellation is pending already.

namespace {

// A run that waits for its answer, over plain stores, and the handle that started it.
struct PausedReview {
    std::optional<ProgramHandle> handle;
    std::optional<ProgramResult> paused;
};

PausedReview pause_run(HostPumpedProgramHost& host, const ProgramVersion& version, const std::string& run_id) {
    PausedReview out;
    auto script = [&]() -> asio::awaitable<int> {
        out.handle = co_await host.runtime->start_async(top_level(version.id(), run_id));
        out.paused = co_await out.handle->wait_async();
        co_return 0;
    };
    host.run(script());
    return out;
}

PausedReview pause_review(HostPumpedProgramHost& host, const std::string& run_id) {
    return pause_run(host, host.review, run_id);
}

std::optional<ContinuationState> stored_state(HostPumpedProgramHost& host, const std::string& run_id) {
    const auto stored = host.fixture.journal->load(kOwner, run_id);
    if (!stored) return std::nullopt;
    return stored->continuation().state;
}

std::uint64_t journal_sequence(HostPumpedProgramHost& host, const std::string& run_id) {
    const auto latest = host.fixture.journal->latest(kOwner, run_id);
    return latest ? latest->sequence : 0;
}

std::uint64_t stored_attempt(HostPumpedProgramHost& host, const std::string& run_id) {
    const auto latest = host.fixture.journal->latest(kOwner, run_id);
    return latest ? latest->continuation.attempt : 0;
}

std::size_t cancelled_terminals(HostPumpedProgramHost& host, const std::string& run_id) {
    std::size_t count = 0;
    for (const auto& event : host.fixture.journal->load_events(kOwner, run_id, 0))
        if (event.kind == ProgramEventKind::Terminal &&
            std::get<ProgramTerminalEvent>(event.payload).status == ProgramTerminalStatus::Cancelled)
            ++count;
    return count;
}

bool pump_until_cancelled(HostPumpedProgramHost& host, const ProgramHandle& handle) {
    return host.hosted.host->run_until([&] {
        const auto result = handle.try_result();
        return result && result->status() == ProgramTerminalStatus::Cancelled;
    });
}

bool has_notice(const ProgramHandle& handle, const std::string& code) {
    for (const auto& notice : handle.notices())
        if (notice.code == code) return true;
    return false;
}

}  // namespace

TEST(ProgramHostPumpedStores, ASynchronousCancelOfAnInterruptedRunIsAcceptedAndRecordedByATaskOfTheLoop) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  run = pause_review(host, "sync-cancel-out-of-turn");
    ASSERT_TRUE(run.handle && run.paused);
    ASSERT_EQ(run.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*run.paused);
    host.stores.transitions->set_pumped(true);
    const auto operations = host.stores.transitions->operations();
    const auto sequence   = journal_sequence(host, "sync-cancel-out-of-turn");

    // Made by the host's thread between its turns: the call neither waits nor is refused.
    EXPECT_TRUE(run.handle->cancel());
    // The task is only queued: it has not taken the coordination lock or read a store when cancel() returns.
    EXPECT_EQ(host.stores.transitions->operations(), operations);
    EXPECT_FALSE(run.handle->cancel()) << "a cancellation that is pending is not scheduled twice";
    EXPECT_EQ(stored_state(host, "sync-cancel-out-of-turn"), ContinuationState::Interrupted);
    ASSERT_TRUE(run.handle->try_result());
    EXPECT_EQ(run.handle->try_result()->status(), ProgramTerminalStatus::Interrupted);

    ASSERT_TRUE(pump_until_cancelled(host, *run.handle)) << "the loop ran out of turns before the run was cancelled";
    EXPECT_EQ(stored_state(host, "sync-cancel-out-of-turn"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-out-of-turn"), 1U);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-out-of-turn"), sequence + 1U);
    EXPECT_FALSE(run.handle->cancel()) << "cancelled already";

    // Nothing is written after the one Cancelled record.
    host.hosted.host->run_until([] { return false; }, 20ms);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-out-of-turn"), 1U);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-out-of-turn"), sequence + 1U);
    EXPECT_TRUE(run.handle->notices().empty());
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

TEST(ProgramHostPumpedStores, ASynchronousCancelInsideAHostTurnIsAcceptedAndRecordedByATaskOfTheLoop) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  run = pause_review(host, "sync-cancel-in-turn");
    ASSERT_TRUE(run.handle && run.paused);
    ASSERT_EQ(run.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*run.paused);
    host.stores.transitions->set_pumped(true);
    const auto sequence = journal_sequence(host, "sync-cancel-in-turn");

    bool          accepted = false;
    bool          again    = true;
    std::uint64_t reads    = 0;
    auto          script   = [&]() -> asio::awaitable<int> {
        const auto before = host.stores.transitions->operations();
        accepted          = run.handle->cancel();
        // The runtime's executor never runs work inline, so the task is queued here too: it has not read the record,
        // and so has not taken the coordination lock, when cancel() returns.
        reads = host.stores.transitions->operations() - before;
        again = run.handle->cancel();
        co_return 0;
    };
    host.run(script());
    EXPECT_TRUE(accepted);
    EXPECT_FALSE(again);
    EXPECT_EQ(reads, 0U);
    ASSERT_TRUE(pump_until_cancelled(host, *run.handle));
    EXPECT_EQ(stored_state(host, "sync-cancel-in-turn"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-in-turn"), 1U);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-in-turn"), sequence + 1U);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

TEST(ProgramHostPumpedStores, ASynchronousCancelOfAReconnectedInterruptedRunIsAccepted) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  run = pause_review(host, "sync-cancel-reconnected");
    ASSERT_TRUE(run.handle && run.paused);
    host.stores.transitions->set_pumped(true);

    std::optional<ProgramHandle> reconnected;
    auto script = [&]() -> asio::awaitable<int> {
        reconnected = co_await host.runtime->reconnect_async(kOwner, "sync-cancel-reconnected");
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(reconnected);
    ASSERT_TRUE(reconnected->try_result());
    EXPECT_EQ(reconnected->try_result()->status(), ProgramTerminalStatus::Interrupted);
    EXPECT_TRUE(reconnected->cancel());
    EXPECT_FALSE(reconnected->cancel());
    ASSERT_TRUE(pump_until_cancelled(host, *reconnected));
    EXPECT_EQ(stored_state(host, "sync-cancel-reconnected"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-reconnected"), 1U);
}

// A run that nothing in the process runs any more is read back from the stores, and its handle holds a different
// control from the one that ran the attempt. It holds the same claim: a second cancel() schedules nothing, and a resume
// is refused until the Cancelled state is written.
TEST(ProgramHostPumpedStores, ASynchronousCancelOfARunReadBackFromTheStoresHoldsTheClaimToo) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("sync-cancel-read-back", host.review);
    auto run = pause_review(host, "sync-cancel-read-back");
    ASSERT_TRUE(run.handle && run.paused);
    const auto paused = *run.paused;
    run.handle.reset();  // no handle keeps the control of the attempt that paused
    host.hosted.host->run_until([] { return false; }, 20ms);
    host.stores.transitions->set_pumped(true);

    std::optional<ProgramHandle> read_back;
    auto script = [&]() -> asio::awaitable<int> {
        read_back = co_await host.runtime->reconnect_async(kOwner, "sync-cancel-read-back");
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(read_back && read_back->try_result());
    EXPECT_EQ(read_back->try_result()->status(), ProgramTerminalStatus::Interrupted);

    EXPECT_TRUE(read_back->cancel());
    EXPECT_FALSE(read_back->cancel()) << "a cancellation that is pending is not scheduled twice";
    std::string                  refusal;
    std::optional<ProgramHandle> resumed;
    auto resume = [&]() -> asio::awaitable<int> {
        try {
            auto answer = answer_for(paused, json{{"decision", "approved"}}, "trace-resume-read-back");
            resumed     = co_await host.runtime->resume_handle_async(kOwner, "sync-cancel-read-back", std::move(answer));
        } catch (const ProgramDiagnosticError& error) {
            refusal = error.diagnostic().code;
        }
        co_return 0;
    };
    host.run(resume());
    EXPECT_FALSE(resumed) << "a run was resumed after its cancellation was accepted";
    EXPECT_EQ(refusal, "P_RESUME_CONFLICT");
    ASSERT_TRUE(pump_until_cancelled(host, *read_back));
    EXPECT_EQ(stored_state(host, "sync-cancel-read-back"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-read-back"), 1U);
    EXPECT_EQ(model_calls.load(), 0);
}

// The resume is started after the call accepted the cancellation, before the loop wrote it. It must not win: the
// Cancelled record is written, and the resume is refused, so no resumed run continues after the accepted call.
namespace {
// `lock_held`: the cancellation of another run is under way, and every cancellation takes the coordination lock first,
// so the task of the accepted cancel() waits behind it. A resume does not take that lock, and would reach the store
// before the task if nothing stopped it.
void resume_after_an_accepted_cancel(bool cancel_inside_the_turn, bool lock_held = false) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("sync-cancel-then-resume", host.review);
    auto run    = pause_review(host, "sync-cancel-then-resume");
    auto holder = pause_review(host, "sync-cancel-lock-holder");
    ASSERT_TRUE(run.handle && run.paused && holder.handle);
    ASSERT_EQ(run.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*run.paused);
    host.stores.transitions->set_pumped(true);
    // The other cancellation takes the lock, and its first read of the store does not complete until the test says so.
    auto hold_the_lock = [&]() -> asio::awaitable<void> { (void)co_await holder.handle->cancel_async(); };
    if (lock_held) host.stores.transitions->hold_next("transition.load");

    bool                         accepted = false;
    std::string                  refusal;
    std::optional<ProgramHandle> resumed;
    auto resume = [&]() -> asio::awaitable<void> {
        try {
            auto answer = answer_for(*run.paused, json{{"decision", "approved"}}, "trace-resume-after-cancel");
            resumed     = co_await host.runtime->resume_handle_async(kOwner, "sync-cancel-then-resume", std::move(answer));
        } catch (const ProgramDiagnosticError& error) {
            refusal = error.diagnostic().code;
        }
    };
    if (cancel_inside_the_turn) {
        auto script = [&]() -> asio::awaitable<int> {
            if (lock_held) asio::co_spawn(co_await asio::this_coro::executor, hold_the_lock, asio::detached);
            accepted = run.handle->cancel();
            co_await resume();
            co_return 0;
        };
        host.run(script());
    } else {
        if (lock_held) asio::co_spawn(host.hosted.loop.executor(), hold_the_lock, asio::detached);
        accepted    = run.handle->cancel();
        auto script = [&]() -> asio::awaitable<int> {
            co_await resume();
            co_return 0;
        };
        host.run(script());
    }
    EXPECT_TRUE(accepted);
    EXPECT_FALSE(resumed) << "a run was resumed after its cancellation was accepted";
    EXPECT_EQ(refusal, "P_RESUME_CONFLICT");
    host.stores.transitions->release_held();
    ASSERT_TRUE(pump_until_cancelled(host, *run.handle));
    EXPECT_EQ(stored_state(host, "sync-cancel-then-resume"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-then-resume"), 1U);
    EXPECT_EQ(model_calls.load(), 0) << "the resumed run would have called the model";
    EXPECT_EQ(tool_runs.load(), 0);

    // Once the state is written the resume is refused for the state.
    refusal.clear();
    auto again = [&]() -> asio::awaitable<int> {
        co_await resume();
        co_return 0;
    };
    host.run(again());
    EXPECT_EQ(refusal, "P_RESUME_STATE");
    EXPECT_FALSE(resumed);
}
}  // namespace

TEST(ProgramHostPumpedStores, AResumeStartedAfterAnAcceptedCancelIsRefusedAndTheRunEndsCancelled) {
    resume_after_an_accepted_cancel(false);
}

TEST(ProgramHostPumpedStores, AResumeInTheSameTurnAsAnAcceptedCancelIsRefusedAndTheRunEndsCancelled) {
    resume_after_an_accepted_cancel(true);
}

TEST(ProgramHostPumpedStores, AResumeThatCouldOvertakeAnAcceptedCancelBehindAnotherCancellationIsRefused) {
    resume_after_an_accepted_cancel(false, true);
}

TEST(ProgramHostPumpedStores, AResumeInTheSameTurnThatCouldOvertakeAnAcceptedCancelIsRefused) {
    resume_after_an_accepted_cancel(true, true);
}

// A resume that is under way when cancel() is called has passed the claim check, and meets the scheduled cancellation at
// the compare-and-swap of the run's record. Whichever turn of the resume the call falls in, one of the two wins: the
// run is never both resumed and cancelled, and a cancellation that lost says so in the handle's notices.
TEST(ProgramHostPumpedStores, AResumeUnderWayAndAnAcceptedCancelHaveOneWinnerWhateverTurnTheCallFallsIn) {
    int cancelled_runs = 0;
    int resumed_runs   = 0;
    for (int turns = 0; turns <= 60; ++turns) {
        SCOPED_TRACE("cancel() after " + std::to_string(turns) + " turns of the resume");
        model_calls.store(0);
        tool_runs.store(0);
        // No tool grant: a resumed run that wins ends without running the tool, which would complete on a thread of its own.
        HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
        auto run = pause_review(host, "sync-cancel-race");
        ASSERT_TRUE(run.handle && run.paused);
        host.stores.transitions->set_pumped(true);

        std::string                  refusal;
        std::optional<ProgramHandle> resumed;
        bool                         resume_ended = false;
        auto                         resume       = [&]() -> asio::awaitable<int> {
            try {
                auto answer = answer_for(*run.paused, json{{"decision", "approved"}}, "trace-resume-race");
                resumed     = co_await host.runtime->resume_handle_async(kOwner, "sync-cancel-race", std::move(answer));
            } catch (const ProgramDiagnosticError& error) {
                refusal = error.diagnostic().code;
            }
            co_return 0;
        };
        host.hosted.loop.spawn(resume(), [&](std::exception_ptr, int) { resume_ended = true; });
        for (int turn = 0; turn < turns; ++turn)
            if (!host.hosted.host->step()) break;
        const bool accepted = run.handle->cancel();
        ASSERT_TRUE(host.hosted.host->run_until([&] { return resume_ended; })) << "the resume did not end";
        // The cancellation task ends: the run is cancelled, or the notice says that the resume won.
        ASSERT_TRUE(host.hosted.host->run_until([&] {
            return stored_state(host, "sync-cancel-race") == ContinuationState::Cancelled ||
                   has_notice(*run.handle, "P_CANCELLATION_NOT_RECORDED");
        })) << "the scheduled cancellation neither recorded the state nor reported that it could not";
        EXPECT_TRUE(accepted);
        if (stored_state(host, "sync-cancel-race") == ContinuationState::Cancelled) {
            ++cancelled_runs;
            EXPECT_FALSE(resumed) << "a run that was resumed was also cancelled";
            EXPECT_TRUE(refusal == "P_RESUME_CONFLICT" || refusal == "P_RESUME_STATE") << refusal;
            EXPECT_EQ(cancelled_terminals(host, "sync-cancel-race"), 1U);
            EXPECT_EQ(model_calls.load(), 0);
        } else {
            ++resumed_runs;
            ASSERT_TRUE(resumed) << "neither a Cancelled record nor a resumed run, refusal: " << refusal;
            EXPECT_TRUE(refusal.empty());
            EXPECT_EQ(cancelled_terminals(host, "sync-cancel-race"), 0U);
            EXPECT_EQ(run.handle->notices().at(0).witness.value("reason", std::string{}), "state_changed");
            ASSERT_TRUE(host.hosted.host->run_until([&] { return resumed->try_result().has_value(); }));
            EXPECT_NE(resumed->try_result()->status(), ProgramTerminalStatus::Cancelled);
            EXPECT_NE(resumed->try_result()->status(), ProgramTerminalStatus::Interrupted);
        }
        EXPECT_EQ(tool_runs.load(), 0);
    }
    // Both outcomes are reached: the cancellation wins while the resume has yet to publish, and loses once it has.
    EXPECT_GT(cancelled_runs, 0);
    EXPECT_GT(resumed_runs, 0);
}

// The error of the scheduled task has no caller to reach. The run stays as it was, interrupted and resumable; the
// handle's notices say that the cancellation was not recorded, and a later cancel() tries again.
namespace {
void a_failed_scheduled_cancellation(const char* failing_operation) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  run = pause_review(host, "sync-cancel-store-failure");
    ASSERT_TRUE(run.handle && run.paused);
    host.stores.transitions->set_pumped(true);
    const auto sequence = journal_sequence(host, "sync-cancel-store-failure");

    host.stores.transitions->fail_next(failing_operation);
    EXPECT_TRUE(run.handle->cancel());
    ASSERT_TRUE(host.hosted.host->run_until([&] { return has_notice(*run.handle, "P_CANCELLATION_NOT_RECORDED"); }))
        << "the failure of the scheduled cancellation was not reported";
    EXPECT_EQ(stored_state(host, "sync-cancel-store-failure"), ContinuationState::Interrupted);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-store-failure"), sequence);
    ASSERT_TRUE(run.handle->try_result());
    EXPECT_EQ(run.handle->try_result()->status(), ProgramTerminalStatus::Interrupted);
    const auto notices = run.handle->notices();
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices[0].witness.value("run_id", std::string{}), "sync-cancel-store-failure");
    EXPECT_EQ(notices[0].witness.value("reason", std::string{}), "store_error");
    EXPECT_NE(notices[0].witness.value("error", std::string{}).find("injected store failure"), std::string::npos)
        << notices[0].witness.dump();

    // The run was not cancelled, so the caller may try again.
    EXPECT_TRUE(run.handle->cancel());
    ASSERT_TRUE(pump_until_cancelled(host, *run.handle));
    EXPECT_EQ(stored_state(host, "sync-cancel-store-failure"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-store-failure"), 1U);
    EXPECT_EQ(run.handle->notices().size(), 1U);
}
}  // namespace

TEST(ProgramHostPumpedStores, AStoreFailureWhileTheScheduledCancelReadsTheRunIsReportedAndLeavesItInterrupted) {
    a_failed_scheduled_cancellation("transition.load");
}

TEST(ProgramHostPumpedStores, AStoreFailureWhileTheScheduledCancelPublishesIsReportedAndLeavesItInterrupted) {
    a_failed_scheduled_cancellation("transition.compare_publish");
}

TEST(ProgramHostPumpedStores, ASynchronousCancelOfARunThatIsOverIsFalseAndWritesNothing) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("sync-cancel-completed", host.review);

    // A run that completed, and the handle of the attempt that paused before it.
    auto run = pause_review(host, "sync-cancel-completed");
    ASSERT_TRUE(run.handle && run.paused);
    host.stores.transitions->set_pumped(true);
    std::optional<ProgramHandle> resumed;
    auto script = [&]() -> asio::awaitable<int> {
        auto answer = answer_for(*run.paused, json{{"decision", "approved"}}, "trace-resume-completed");
        resumed     = co_await host.runtime->resume_handle_async(kOwner, "sync-cancel-completed", std::move(answer));
        (void)co_await resumed->wait_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(resumed && resumed->try_result());
    ASSERT_EQ(resumed->try_result()->status(), ProgramTerminalStatus::Completed) << failure_text(*resumed->try_result());

    host.hosted.host->run_until([] { return false; }, 20ms);  // what the finished run still does with its store
    const auto operations = host.stores.transitions->operations();
    const auto sequence   = journal_sequence(host, "sync-cancel-completed");
    EXPECT_FALSE(resumed->cancel());
    host.hosted.host->run_until([] { return false; }, 20ms);
    EXPECT_EQ(host.stores.transitions->operations(), operations) << "a run that is over costs the cancel no store call";
    EXPECT_EQ(stored_state(host, "sync-cancel-completed"), ContinuationState::Completed);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-completed"), sequence);

    // The handle of the attempt that paused still holds its Interrupted result, which the call cannot know is stale:
    // it accepts, and the task finds that the run is not interrupted any more. Nothing is written, and the notice says so.
    EXPECT_TRUE(run.handle->cancel());
    ASSERT_TRUE(host.hosted.host->run_until([&] { return has_notice(*run.handle, "P_CANCELLATION_NOT_RECORDED"); }));
    EXPECT_EQ(stored_state(host, "sync-cancel-completed"), ContinuationState::Completed);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-completed"), sequence);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-completed"), 0U);
    EXPECT_EQ(run.handle->notices().at(0).witness.value("reason", std::string{}), "state_changed");
}

TEST(ProgramHostPumpedStores, ACancelledRunIsNotCancelledAgainBySynchronousCancel) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  run = pause_review(host, "sync-cancel-after-async");
    ASSERT_TRUE(run.handle && run.paused);
    host.stores.transitions->set_pumped(true);
    bool applied = false;
    auto script  = [&]() -> asio::awaitable<int> {
        applied = co_await run.handle->cancel_async();
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(applied);
    host.hosted.host->run_until([] { return false; }, 20ms);  // what the cancelled run still does with its store
    const auto operations = host.stores.transitions->operations();
    const auto sequence   = journal_sequence(host, "sync-cancel-after-async");
    EXPECT_FALSE(run.handle->cancel());
    host.hosted.host->run_until([] { return false; }, 20ms);
    EXPECT_EQ(host.stores.transitions->operations(), operations);
    EXPECT_EQ(journal_sequence(host, "sync-cancel-after-async"), sequence);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-after-async"), 1U);
}

// The loop goes before the task runs: the run keeps its Interrupted record, nothing crashes, and the work queued on the
// loop is destroyed with it.
TEST(ProgramHostPumpedStores, AnAcceptedCancelWhoseTaskNeverRunsLeavesTheRunInterrupted) {
    std::shared_ptr<ProgramTransitionStore>        journal;
    std::shared_ptr<neograph::host_test::ManualExecutor> executor;
    {
        HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
        host.drain_on_destroy = false;
        journal               = host.fixture.journal.shared();
        executor              = host.hosted.host;
        auto run              = pause_review(host, "sync-cancel-loop-gone");
        ASSERT_TRUE(run.handle && run.paused);
        host.stores.transitions->set_pumped(true);
        EXPECT_TRUE(run.handle->cancel());
    }
    // The host runs the turn it had queued for the loop, which is gone: that only lets go of the loop's state.
    executor->run_until([] { return false; }, 20ms);
    test_support::TransitionStoreHandle after(journal);
    const auto stored = after->load(kOwner, "sync-cancel-loop-gone");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->continuation().state, ContinuationState::Interrupted);
}

// The host refuses the post that asks for a turn of the loop (HostLoop allows that for an executor that is stopping).
// The task of the accepted cancel() is queued on the loop all the same, together with its claim: it waits for the next
// wake, keeps the claim meanwhile, and records the state and releases the claim when it runs.
TEST(ProgramHostPumpedStores, AnAcceptedCancelWhoseTurnTheHostRefusedWaitsForTheNextWakeAndThenClearsItsClaim) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    host.grant_tool("sync-cancel-refused-turn", host.review);
    auto run = pause_review(host, "sync-cancel-refused-turn");
    ASSERT_TRUE(run.handle && run.paused);
    ASSERT_EQ(run.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*run.paused);
    host.stores.transitions->set_pumped(true);

    host.hosted.host->refuse_posts(true);
    EXPECT_TRUE(run.handle->cancel()) << "the task is queued on the loop whether or not the host took a turn";
    EXPECT_EQ(host.hosted.host->pending(), 0u) << "the host took no turn";
    EXPECT_FALSE(run.handle->cancel()) << "the claim stands although no turn was taken";

    // The host takes posts again, but nothing has woken the loop: nothing runs and the run is as it was.
    host.hosted.host->refuse_posts(false);
    EXPECT_FALSE(host.hosted.host->step()) << "no turn is queued: the task waits for a wake";
    EXPECT_EQ(stored_state(host, "sync-cancel-refused-turn"), ContinuationState::Interrupted);

    // The next wake (here the start of a resume on the loop) runs the queued task first. It holds the claim, so the
    // resume is refused for it; once the state is written the claim is released and the resume is refused for the state.
    std::string                  refusal;
    std::optional<ProgramHandle> resumed;
    auto resume = [&]() -> asio::awaitable<int> {
        try {
            auto answer = answer_for(*run.paused, json{{"decision", "approved"}}, "trace-resume-refused-turn");
            resumed     = co_await host.runtime->resume_handle_async(kOwner, "sync-cancel-refused-turn", std::move(answer));
        } catch (const ProgramDiagnosticError& error) {
            refusal = error.diagnostic().code;
        }
        co_return 0;
    };
    host.run(resume());
    EXPECT_FALSE(resumed) << "a run was resumed after its cancellation was accepted";
    EXPECT_EQ(refusal, "P_RESUME_CONFLICT");
    ASSERT_TRUE(pump_until_cancelled(host, *run.handle));
    EXPECT_EQ(stored_state(host, "sync-cancel-refused-turn"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-refused-turn"), 1U);
    refusal.clear();
    host.run(resume());
    EXPECT_FALSE(resumed);
    EXPECT_EQ(refusal, "P_RESUME_STATE") << "the claim was released when the task ended";
    EXPECT_TRUE(run.handle->notices().empty());
    EXPECT_EQ(model_calls.load(), 0);
    EXPECT_EQ(tool_runs.load(), 0);
}

// ---- A handle of an earlier attempt, and reconcile ----
//
// A handle holds the result of the attempt it came from. After the run was resumed through another handle, the first
// one still holds Interrupted, so a cancel() on it looks like the cancellation of an interrupted run. It must not
// claim the run while a later attempt executes, and what it cancels once the run is interrupted again is the record
// that is stored, under that record's attempt: the attempt of a run only grows.

namespace {

void a_handle_of_an_earlier_attempt(std::shared_ptr<ProgramTransitionStore> journal, const std::string& run_id) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions, false, {}, {}, std::move(journal));
    auto                  first = pause_run(host, host.two_pause, run_id);
    ASSERT_TRUE(first.handle && first.paused);
    ASSERT_EQ(first.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*first.paused);
    ASSERT_EQ(first.paused->attempt(), 1U);
    host.stores.transitions->set_pumped(true);

    // Resume it. The resumed attempt is registered and has not run yet when resume_handle_async returns: the run
    // executes, and the first handle is the handle of an earlier attempt.
    bool                         stale_accepted = true;
    std::optional<ProgramHandle> second;
    std::optional<ProgramResult> second_pause;
    auto script = [&]() -> asio::awaitable<int> {
        auto answer = answer_for(*first.paused, json{{"decision", "one"}}, "trace-earlier-attempt-1");
        second      = co_await host.runtime->resume_handle_async(kOwner, run_id, std::move(answer));
        stale_accepted = first.handle->cancel();
        second_pause   = co_await second->wait_async();
        co_return 0;
    };
    host.run(script());
    EXPECT_FALSE(stale_accepted) << "the run executes in a later attempt: this handle has nothing to cancel";
    ASSERT_TRUE(second && second_pause);
    ASSERT_EQ(second_pause->status(), ProgramTerminalStatus::Interrupted) << failure_text(*second_pause);
    EXPECT_EQ(second_pause->attempt(), 2U);
    host.hosted.host->run_until([] { return false; }, 20ms);
    EXPECT_TRUE(first.handle->notices().empty()) << "no cancellation was scheduled, so none is reported";
    EXPECT_EQ(stored_state(host, run_id), ContinuationState::Interrupted);
    EXPECT_EQ(stored_attempt(host, run_id), 2U);

    // The second attempt paused too, and the first handle cannot tell. The cancellation is of the run that is stored,
    // and it is written under the attempt of that record.
    const auto sequence = journal_sequence(host, run_id);
    EXPECT_TRUE(first.handle->cancel());
    ASSERT_TRUE(pump_until_cancelled(host, *first.handle));
    EXPECT_EQ(stored_state(host, run_id), ContinuationState::Cancelled);
    EXPECT_EQ(journal_sequence(host, run_id), sequence + 1U);
    EXPECT_EQ(stored_attempt(host, run_id), 2U) << "the attempt of the run went backwards";
    const auto stored = host.fixture.journal->load(kOwner, run_id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->continuation().attempt, 2U);
    ASSERT_TRUE(stored->terminal_result());
    EXPECT_EQ(stored->terminal_result()->attempt(), 2U);
    const auto events = host.fixture.journal->load_events(kOwner, run_id, 0);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.back().kind, ProgramEventKind::Terminal);
    EXPECT_EQ(events.back().attempt, 2U);
    EXPECT_EQ(cancelled_terminals(host, run_id), 1U);
    EXPECT_EQ(effect_runs.load(), 0);
}

}  // namespace

TEST(ProgramHostPumpedStores, AHandleOfAnEarlierAttemptDoesNotCancelARunThatExecutesAndKeepsTheStoredAttempt) {
    a_handle_of_an_earlier_attempt({}, "earlier-attempt-memory");
}

#ifdef NEOGRAPH_PROGRAM_TESTS_HAVE_SQLITE
TEST(ProgramHostPumpedStores, AHandleOfAnEarlierAttemptDoesNotCancelARunThatExecutesAndKeepsTheStoredAttemptOnSqlite) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("neograph-earlier-attempt-" + graph::Checkpoint::generate_id() + ".db");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.string() + "-wal", ignored);
            std::filesystem::remove(path.string() + "-shm", ignored);
        }
    } cleanup{path};
    a_handle_of_an_earlier_attempt(std::make_shared<SQLiteProgramTransitionStore>(path.string()),
                                   "earlier-attempt-sqlite");
}
#endif

// reconcile consumes an interrupted run as resume does: with the outcome of its effect declared unknown it moves the
// record to AmbiguousEffect, which cancel_async leaves alone. It does not take the coordination lock either, so with
// the task of an accepted cancel() waiting behind another cancellation it would reach the store first. It is refused
// while the accepted cancellation is pending.
TEST(ProgramHostPumpedStores, AReconcileThatCouldOvertakeAnAcceptedCancelBehindAnotherCancellationIsRefused) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Transitions);
    auto                  control = pause_run(host, host.effect, "reconcile-control");
    auto                  holder  = pause_run(host, host.effect, "reconcile-lock-holder");
    auto                  run     = pause_run(host, host.effect, "sync-cancel-then-reconcile");
    ASSERT_TRUE(control.paused && holder.handle && run.handle && run.paused);
    ASSERT_EQ(run.paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*run.paused);
    ASSERT_TRUE(run.paused->interrupt() && run.paused->interrupt()->pending_effect);
    host.stores.transitions->set_pumped(true);

    // The control: with no cancellation pending, the same reconcile of a paused run goes through.
    std::optional<ProgramResult> ambiguous;
    auto reconcile_control = [&]() -> asio::awaitable<int> {
        ProgramEffectResolution resolution{pending_call_id(*control.paused), ProgramEffectReconciliation::Unknown,
                                           std::nullopt, "trace-reconcile-control", {}};
        auto handle = co_await host.runtime->reconcile_async(kOwner, "reconcile-control", std::move(resolution));
        ambiguous   = co_await handle.wait_async();
        co_return 0;
    };
    host.run(reconcile_control());
    ASSERT_TRUE(ambiguous);
    EXPECT_EQ(ambiguous->status(), ProgramTerminalStatus::AmbiguousEffect) << failure_text(*ambiguous);
    EXPECT_EQ(stored_state(host, "reconcile-control"), ContinuationState::AmbiguousEffect);

    bool                         accepted = false;
    std::string                  refusal;
    std::optional<ProgramHandle> reconciled;
    // The other cancellation takes the lock, and its first read of the store does not complete until the test says so.
    auto hold_the_lock = [&]() -> asio::awaitable<void> { (void)co_await holder.handle->cancel_async(); };
    host.stores.transitions->hold_next("transition.load");
    auto reconcile = [&]() -> asio::awaitable<int> {
        asio::co_spawn(co_await asio::this_coro::executor, hold_the_lock, asio::detached);
        accepted = run.handle->cancel();
        try {
            ProgramEffectResolution resolution{pending_call_id(*run.paused), ProgramEffectReconciliation::Unknown,
                                               std::nullopt, "trace-reconcile-after-cancel", {}};
            reconciled = co_await host.runtime->reconcile_async(kOwner, "sync-cancel-then-reconcile", std::move(resolution));
        } catch (const ProgramDiagnosticError& error) {
            refusal = error.diagnostic().code;
        }
        co_return 0;
    };
    host.run(reconcile());
    EXPECT_TRUE(accepted);
    EXPECT_FALSE(reconciled) << "the run was reconciled after its cancellation was accepted";
    EXPECT_EQ(refusal, "P_RECONCILE_CONFLICT");
    host.stores.transitions->release_held();
    ASSERT_TRUE(pump_until_cancelled(host, *run.handle));
    EXPECT_EQ(stored_state(host, "sync-cancel-then-reconcile"), ContinuationState::Cancelled);
    EXPECT_EQ(cancelled_terminals(host, "sync-cancel-then-reconcile"), 1U);
    EXPECT_TRUE(run.handle->notices().empty());
    EXPECT_EQ(effect_runs.load(), 3) << "each run asked for its effect once, and none ran again";
}

// ---- The whole lifecycle ----

TEST(ProgramHostPumpedStores, AProgramLifecycleRunsInHostTurnsWithoutBlockingOrBeingRefused) {
    HostPumpedProgramHost host;
    host.grant_tool("review-1", host.review);
    host.grant_tool("fork-target", host.review);
    // Activate one version, so that the catalog has an activation to read.
    ASSERT_EQ(host.setup_catalog->activate(kOwner, host.review.id(), 0), ProgramActivationResult::Activated);
    host.stores.pump_all();

    std::optional<ProgramVersion>           resolved;
    std::optional<ProgramActivationBinding> active;
    std::optional<ProgramResult>            paused, resumed, fork_source, forked, expanded;
    std::optional<ProgramRunRecord>         record;
    auto script = [&]() -> asio::awaitable<int> {
        // The catalog: a version by identity, and the active one.
        resolved = co_await host.catalog->resolve_version_async(kOwner, host.review.id());
        active   = co_await host.catalog->active_binding_async(kOwner);

        // Start; an LLM node with a fake provider ran in the first step of the model call, after an approval pause.
        auto first = co_await host.runtime->start_async(top_level(host.review.id(), "review-1"));
        paused     = co_await first.wait_async();

        // Resume: the model is called, the tool call is gated by the grant, the run completes.
        auto answer         = answer_for(*paused, json{{"decision", "approved"}}, "trace-resume");
        auto resumed_handle = co_await host.runtime->resume_handle_async(kOwner, "review-1", std::move(answer));
        resumed             = co_await resumed_handle.wait_async();
        record              = co_await resumed_handle.snapshot_async();

        // Fork a second paused run at its checkpoint.
        auto second = co_await host.runtime->start_async(top_level(host.review.id(), "fork-source"));
        fork_source = co_await second.wait_async();
        ExactProgramCheckpointReference reference{fork_source->run_id(), fork_source->checkpoint()->checkpoint_id};
        auto invocation  = fork_invocation(host.review, *fork_source, "fork-target");
        auto fork_answer = answer_for(*fork_source, json{{"decision", "forked"}}, "trace-fork");
        auto fork_handle = co_await host.runtime->fork_async(std::move(reference), std::move(invocation),
                                                              std::move(fork_answer));
        forked           = co_await fork_handle.wait_async();

        // A child spawn: the parent expands a task graph into two child runs.
        ProgramInvocation parent_invocation{json{{"item", "seed"}}, RunBudget{10000, 1000, 1000, 1, 2, 20, 1, 2, 2},
                                            "trace-parent", {}};
        parent_invocation.requested_run_id = "parent-1";
        auto parent_handle = co_await host.runtime->start_async(kOwner, host.parent, std::move(parent_invocation));
        expanded           = co_await parent_handle.wait_async();
        co_return 0;
    };
    host.run(script());

    ASSERT_TRUE(resolved && active && paused && resumed && fork_source && forked && expanded && record);
    EXPECT_EQ(resolved->id(), host.review.id());
    EXPECT_EQ(active->version.id(), host.review.id());
    EXPECT_EQ(paused->status(), ProgramTerminalStatus::Interrupted) << failure_text(*paused);
    EXPECT_EQ(resumed->status(), ProgramTerminalStatus::Completed) << failure_text(*resumed);
    EXPECT_EQ(record->continuation().state, ContinuationState::Completed);
    EXPECT_EQ(fork_source->status(), ProgramTerminalStatus::Interrupted) << failure_text(*fork_source);
    EXPECT_EQ(forked->status(), ProgramTerminalStatus::Completed) << failure_text(*forked);
    EXPECT_EQ(expanded->status(), ProgramTerminalStatus::Completed) << failure_text(*expanded);
    EXPECT_EQ(channel(*resumed, "value"), "approved");
    EXPECT_EQ(channel(*forked, "value"), "forked");
    EXPECT_EQ(approvals_asked.load(), 2);
    EXPECT_EQ(model_calls.load(), 2);
    EXPECT_EQ(tool_runs.load(), 2) << "the granted runs ran their tool once each";
    EXPECT_EQ(child_runs.load(), 2);

    // Every store the lifecycle used took turns of the loop, and each call was completed by the host's thread.
    EXPECT_GT(host.stores.transitions->suspended(), 0U);
    EXPECT_GT(host.stores.programs->suspended(), 0U);
    EXPECT_GT(host.stores.grants->suspended(), 0U);
    EXPECT_GT(host.stores.fragments->suspended(), 0U);
    EXPECT_COMPLETED_ON_THE_HOST_THREAD(host);
}

// ---- Catalog and module reads, and what stays refused ----

TEST(ProgramHostPumpedStores, ACatalogReadAndTheModuleResolverAwaitTheirStores) {
    HostPumpedProgramHost host(HostPumpedProgramHost::Programs | HostPumpedProgramHost::Modules);
    ASSERT_EQ(host.setup_catalog->activate(kOwner, host.review.id(), 0), ProgramActivationResult::Activated);

    ProgramModuleData dependency_data;
    dependency_data.owner_scope          = kOwner;
    dependency_data.coordinate           = ModuleCoordinate{"test", "dependency", "1.0.0", ""};
    dependency_data.attestation_id       = "attestation:test";
    dependency_data.allowed_capabilities = {"read"};
    dependency_data.declared_effects     = {"tool"};
    const auto dependency                = ProgramModule::create(std::move(dependency_data));
    ProgramModuleData root_data;
    root_data.owner_scope          = kOwner;
    root_data.coordinate           = ModuleCoordinate{"test", "root", "1.0.0", ""};
    root_data.attestation_id       = "attestation:test";
    root_data.dependencies         = {dependency.coordinate()};
    root_data.allowed_capabilities = {"read"};
    root_data.declared_effects     = {"tool"};
    const auto root                = ProgramModule::create(std::move(root_data));
    host.inner_modules->publish_sync(dependency);
    host.inner_modules->publish_sync(root);
    PolicySnapshotBuilder policy_builder;
    policy_builder.id("resolver-policy")
        .semantic_version("1.0.0")
        .owner_scope(kOwner)
        .admission_profile(host.fixture.profile)
        .allow_module_digest(root.id())
        .allow_module_digest(dependency.id())
        .allow_capability("read")
        .allow_effect("tool")
        .budget_ceiling(BudgetLimits{10, 10, 10, 1, 10, 10, 1, 1, 1});
    const auto policy = std::move(policy_builder).build();

    host.stores.pump_all();
    ModuleResolver resolver(host.wrapped_modules);
    std::optional<ProgramVersion>           found, listed;
    std::optional<ProgramActivation>        activation;
    std::optional<ProgramActivationBinding> binding;
    std::optional<ModuleResolution>         resolution;
    auto script = [&]() -> asio::awaitable<int> {
        // A version is read from the store once; the catalog then knows it.
        listed     = co_await host.catalog->resolve_version_async(kOwner, host.child.id());
        found      = co_await host.catalog->find_version_async(kOwner, host.child.id());
        activation = co_await host.catalog->activation_async(kOwner);
        binding    = co_await host.catalog->active_binding_async(kOwner);
        resolution = co_await resolver.resolve_async(root.coordinate(), policy);
        co_return 0;
    };
    host.run(script());
    ASSERT_TRUE(found) << "find_version";
    ASSERT_TRUE(listed) << "resolve_version";
    ASSERT_TRUE(activation) << "activation";
    ASSERT_TRUE(binding) << "active_binding";
    ASSERT_TRUE(resolution) << "module resolution";
    EXPECT_EQ(found->id(), host.child.id());
    EXPECT_EQ(listed->id(), host.child.id());
    EXPECT_EQ(activation->active_version_id(), host.review.id());
    EXPECT_EQ(binding->version.id(), host.review.id());
    EXPECT_EQ(resolution->modules.size(), 2U);
    EXPECT_GT(host.stores.programs->suspended(), 0U);
    EXPECT_GT(host.stores.modules->suspended(), 0U);
    EXPECT_EQ(host.stores.modules->off_thread() + host.stores.programs->off_thread(), 0U);
}

TEST(ProgramHostPumpedStores, SynchronousCallsInsideAHostTurnAreRefusedBeforeTheyTouchAStore) {
    HostPumpedProgramHost host;
    host.stores.pump_all();
    const auto before = host.stores.operations();

    std::vector<std::string> refused;
    auto script = [&]() -> asio::awaitable<int> {
        try {
            (void)host.catalog->resolve_version(kOwner, host.review.id());
        } catch (const ProgramBlockingCallRefused& error) {
            refused.push_back(error.operation().empty() ? "catalog" : error.operation());
        }
        try {
            (void)host.runtime->start(top_level(host.review.id(), "never-started"));
        } catch (const ProgramBlockingCallRefused& error) {
            refused.push_back(error.operation());
        }
        co_return 0;
    };
    host.run(script());
    ASSERT_EQ(refused.size(), 2U);
    EXPECT_EQ(refused[1], "ProgramRuntime::start");
    EXPECT_EQ(host.stores.operations(), before) << "a refused call must not touch a store";
}
