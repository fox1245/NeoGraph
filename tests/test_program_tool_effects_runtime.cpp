// Durable mediated Tool effects through ProgramRuntime.
//
// The real SQLiteToolEffectBroker is composed with a real ProgramRuntime: an
// engine-owned node dispatches a multi-call assistant batch against one
// host-bound Tool, the grant is re-admitted per attempt through the durable
// SQLite grant store, and every reopen builds new stores, a new catalog/engine
// generation, a new runtime and a new Tool instance. Effects are counted in an
// external ledger that the broker never touches, so "no re-execution" is
// observed independently of the broker's own receipts.

#include <neograph/async/run_sync.h>
#include <neograph/graph/engine.h>
#include <neograph/graph/node.h>
#include <neograph/graph/run_context.h>
#include <neograph/graph/sqlite_checkpoint.h>
#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/program.h>
#include <neograph/program/sqlite_core_tool_grant_store.h>
#include <neograph/program/sqlite_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/sqlite_runtime_stores.h>
#include <neograph/tool_dispatch.h>
#include "fixtures/tool_effect_ledger.h"
#include "fixtures/tool_effect_sync_fault.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace neograph;
using namespace neograph::program;
using namespace neograph::test::tool_effects;
namespace fs = std::filesystem;

constexpr const char* kOwner = "tenant:effects";
std::string digest(char value) { return "sha256:" + std::string(64, value); }

// ── Host: durable stores, grant resolver, broker, per-run plans ──────────

// The existing LedgerTool calls this fence only after its independent SQLite
// INSERT has committed. The node supplies the actual Program/Core execution
// token and deadline; no test manufactures a Tool effect identity or outcome.
struct PostCommitStopFence {
    void bind(const ToolExecutionContext& execution) {
        std::lock_guard lock(mutex);
        token = execution.cancel_token;
        deadline = execution.deadline;
    }

    void after_effect() {
        std::shared_ptr<graph::CancelToken> execution_token;
        {
            std::lock_guard lock(mutex);
            execution_token = token;
            if (!execution_token || !deadline)
                throw std::logic_error("Program execution token/deadline was not supplied");
            committed_at = std::chrono::steady_clock::now();
            stopped_before_commit = execution_token->is_cancelled();
            committed = true;
            cv.notify_all();
        }
        // Native subscription: the real Program timer or ProgramHandle::cancel
        // wakes the blocking Tool without sleeping or borrowing an Asio slot.
        std::stop_callback subscription(execution_token->stop_token(), [this] {
            std::lock_guard lock(mutex);
            stopped_at = std::chrono::steady_clock::now();
            stopped = true;
            cv.notify_all();
        });
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [this] { return stopped || released; });
        }
        // Cooperative cancellation, not a synthetic lost-response exception.
        // The controller must classify the actually cancelled execution, and
        // the broker must retain its already-committed unresolved marker.
        execution_token->throw_if_cancelled("after external ledger commit");
    }

    bool wait_until_committed() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(20), [this] { return committed; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::shared_ptr<graph::CancelToken> token;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::chrono::steady_clock::time_point committed_at, stopped_at;
    bool committed = false, stopped = false, stopped_before_commit = false, released = false;
};

// Declared after the handle, so even a fatal assertion releases a blocked
// worker before EffectHost tears down its runtime and joins owned operations.
struct PostCommitStopRelease {
    std::shared_ptr<PostCommitStopFence> fence;
    ~PostCommitStopRelease() { fence->release(); }
};

struct RunPlan {
    std::vector<ToolCall> calls;
    bool interrupt_after = false;
    std::shared_ptr<PostCommitStopFence> post_commit_stop;
};

struct NodeObservation {
    std::string run;
    std::vector<ChatMessage> messages;
};

struct NodeDispatch {
    std::string run, thread, task, grant, fingerprint;
    std::uint64_t attempt = 0, generation = 0;
};

struct OpenOptions {
    std::string executable = kExecutable;
    // Bind the broker to a Tool instance the engine's node does not use.
    bool bind_foreign_tool = false;
    std::function<void()> after_effect;
};

class EffectHost;

class EffectBatchNode final : public graph::GraphNode {
public:
    EffectBatchNode(std::string name, EffectHost* host, std::uint64_t generation)
        : name_(std::move(name)), host_(host), generation_(generation) {}
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override;
    std::string get_name() const override { return name_; }

private:
    std::string name_;
    EffectHost* host_;
    std::uint64_t generation_;
};

class EffectHost {
public:
    explicit EffectHost(std::shared_ptr<Rendezvous> rendezvous = {})
        : rendezvous_(std::move(rendezvous)), registry_(make_registry()),
          profile_(make_profile(registry_)), policy_(make_policy(profile_)) {
        static std::atomic<std::uint64_t> next{0};
        root_ = fs::canonical(fs::temp_directory_path()) /
                ("ng-program-tool-effects-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(++next));
        if (!fs::create_directory(root_)) throw std::runtime_error("cannot reserve test directory");
        ToolExecutionPolicy widened;
        widened.concurrency = ToolConcurrency::Capacity;
        widened.capacity = 16;
        widened.effect = ToolEffectClass::ExternalWrite;
        controller_->policies()->upsert("ledger", widened);
        open();
    }
    ~EffectHost() {
        close();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }
    EffectHost(const EffectHost&) = delete;
    EffectHost& operator=(const EffectHost&) = delete;

    std::string journal_path() const { return (root_ / "effects.sqlite").string(); }
    std::string ledger_path() const { return (root_ / "ledger.sqlite").string(); }

    // Everything a process restart loses: stores, catalog, engines, runtime,
    // broker connection and the Tool instance itself.
    void close() {
        runtime_.reset();
        grant_store_.reset();
        broker_.reset();
        tool_.reset();
        foreign_tool_.reset();
        version_.reset();
        catalog_.reset();
        engines_.reset();
        program_store_.reset();
        transitions_.reset();
        checkpoints_.reset();
    }

    void open(OpenOptions options = {}) {
        close();
        tool_ = std::make_unique<LedgerTool>(ledger_path(), rendezvous_, nullptr,
                                             std::move(options.after_effect));
        foreign_tool_ = std::make_unique<LedgerTool>(ledger_path(), rendezvous_);
        Tool* bound = options.bind_foreign_tool ? foreign_tool_.get() : tool_.get();
        broker_ = std::make_shared<SQLiteToolEffectBroker>(
            journal_path(), std::vector<SQLiteToolExecutableBinding>{{bound, options.executable}});
        program_store_ = std::make_shared<SQLiteProgramStore>((root_ / "programs.sqlite").string());
        transitions_ = std::make_shared<SQLiteProgramTransitionStore>(
            (root_ / "transitions.sqlite").string());
        checkpoints_ = std::make_shared<graph::SqliteCheckpointStore>(
            (root_ / "checkpoints.sqlite").string());
        grant_store_ = std::make_shared<SQLiteProgramCoreToolGrantStore>(
            (root_ / "grants.sqlite").string());
        engines_ = std::make_shared<EngineGenerationCache>();
        catalog_ = std::make_shared<ProgramCatalog>(
            CatalogConfig{program_store_, registry_, engines_, "program-tool-effects/v1"});
        RuntimeConfig config{catalog_, checkpoints_, {}, transitions_, 8, {}};
        config.core_tool_grant_resolver = make_durable_core_tool_grant_resolver(
            grant_store_, [this](const ProgramCoreToolGrantRecord& record)
                              -> std::optional<ProgramCoreToolGrant> {
                ProgramCoreToolGrant grant;
                grant.owner_scope = record.owner_scope;
                grant.program_version_id = record.program_version_id;
                grant.run_id = record.run_id;
                grant.operation_id = record.operation_id;
                grant.attempt = record.attempt;
                grant.binding_fingerprint = record.binding_fingerprint;
                grant.grant_id = record.grant_id;
                grant.gate = [](ToolCall call, ToolGateContext)
                    -> asio::awaitable<ToolDecision> {
                    if (call.arguments.find("\"deny\":true") != std::string::npos)
                        co_return ToolDecision::deny("host policy denies this call");
                    co_return ToolDecision::allow();
                };
                grant.controller = controller_;
                grant.effect_broker = broker_;
                return grant;
            });
        runtime_ = std::make_unique<ProgramRuntime>(std::move(config));
        version_ = admit();
    }

    ProgramVersion admit() {
        ProgramCompiler compiler(registry_, {"program-tool-effects/v1"});
        const auto bundle = compiler.compile(
            ProgramSource::from_cpp_builder("test:tool-effects", PROGRAM_SCHEMA_VERSION_V1,
                                            program_document()));
        return catalog_->admit(bundle, ProgramAdmission{kOwner, profile_, policy_, {}});
    }

    const ProgramVersion& version() const { return *version_; }
    ProgramRuntime& runtime() { return *runtime_; }
    LedgerTool& tool() { return *tool_; }
    std::shared_ptr<ToolExecutionController> controller() const { return controller_; }

    std::string grant_for(const std::string& run) const { return "grant-" + run; }

    // The trusted host re-admits its authority for one exact attempt.
    ProgramCoreToolGrantAdmission admit_grant(const std::string& run, std::uint64_t attempt,
                                              std::string grant_id = {}) {
        return grant_store_->admit(ProgramCoreToolGrantRecord{
            kOwner, version().id(), run, "root", attempt,
            capability_binding_receipt_root(
                version().core_materialization_receipt().capability_bindings),
            grant_id.empty() ? grant_for(run) : std::move(grant_id)});
    }

    void set_plan(const std::string& run, RunPlan plan) {
        std::lock_guard lock(mutex_);
        plans_[run] = std::move(plan);
    }
    RunPlan plan_for(const std::string& run) {
        std::lock_guard lock(mutex_);
        return plans_.at(run);
    }
    void observe(NodeObservation observation) {
        std::lock_guard lock(mutex_);
        observations_.push_back(std::move(observation));
    }
    std::vector<NodeObservation> observations(const std::string& run) {
        std::lock_guard lock(mutex_);
        std::vector<NodeObservation> selected;
        for (const auto& observation : observations_)
            if (observation.run == run) selected.push_back(observation);
        return selected;
    }
    void record_dispatch(NodeDispatch dispatch) {
        std::lock_guard lock(mutex_);
        dispatches_.push_back(std::move(dispatch));
    }
    std::vector<NodeDispatch> dispatches(const std::string& run) {
        std::lock_guard lock(mutex_);
        std::vector<NodeDispatch> selected;
        for (const auto& dispatch : dispatches_)
            if (dispatch.run == run) selected.push_back(dispatch);
        return selected;
    }
    Tool* node_tool() { return tool_.get(); }

    ProgramHandle start(const std::string& run, std::string trace = "trace",
                        std::string grant_id = {}) {
        if (admit_grant(run, 1, std::move(grant_id)) != ProgramCoreToolGrantAdmission::Admitted)
            throw std::runtime_error("grant already present for " + run);
        ProgramInvocation invocation{json::object(), run_budget(), std::move(trace), {}};
        invocation.requested_run_id = run;
        return runtime_->start(kOwner, version(), std::move(invocation));
    }

    ProgramResult resume(const ProgramResult& interrupted, std::string grant_id = {},
                         bool admit_next = true) {
        const auto interrupt = interrupted.interrupt();
        if (!interrupt) throw std::invalid_argument("result is not interrupted");
        const auto pending = interrupt->pending_input ? interrupt->pending_input->call_id()
                                                      : interrupt->pending_effect->call_id();
        const auto next = interrupted.attempt() + 1;
        if (admit_next && admit_grant(interrupted.run_id(), next, std::move(grant_id)) !=
                              ProgramCoreToolGrantAdmission::Admitted)
            throw std::runtime_error("next attempt grant was not admitted");
        return runtime_
            ->resume(kOwner, interrupted.run_id(),
                     ProgramResume{json::object(), "resume-trace", {}, pending})
            .wait();
    }

    static RunBudget run_budget() { return RunBudget{10000, 1000, 1000, 1, 1, 20, 0, 0, 0}; }

    std::shared_ptr<Rendezvous> rendezvous_;

private:
    static json program_document() {
        json definition{
            {"schema_version", 1},
            {"name", "main"},
            {"channels", json{{"value", json{{"reducer", "effects-overwrite"}, {"initial", ""}}}}},
            {"nodes", json{{"work", json{{"type", "effects-batch"}}}}},
            {"edges", json::array({json{{"from", "__start__"}, {"to", "work"}},
                                   json{{"from", "work"}, {"to", "__end__"}}})},
            {"conditional_edges", json::array()}};
        json budget = json::array({
            json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 10000}},
            json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 1000}},
            json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 1000}},
            json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 1}},
            json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
            json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
            json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
            json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
            json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}},
        });
        return json{{"program_schema_version", 1},
                    {"input_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
                    {"output_contract", json{{"schema_version", 1}, {"schema", json::object()}}},
                    {"root", json{{"op", "call_core"}, {"name", "main"},
                                  {"definition", std::move(definition)}}},
                    {"declared_budget_requirements", std::move(budget)}};
    }

    static ExecutableManifest manifest(ExecutableKind kind, std::string name, char implementation) {
        return ExecutableManifest{{kind, std::move(name), "1.0.0", digest(implementation)},
                                  EffectMode::Brokered, "attestation:test", {}, {}, {}};
    }

    RegistrySnapshot make_registry() {
        RegistrySnapshotBuilder builder;
        builder.add_node(
            manifest(ExecutableKind::Node, "effects-batch", 'a'),
            [this](const std::string& name, const json&, const graph::NodeContext&) {
                return std::make_unique<EffectBatchNode>(name, this, ++next_generation_);
            },
            json{{"type", "object"}},
            json{{"writes", json::array({"value"})}, {"exports", json::array({"value"})}});
        builder.add_reducer(manifest(ExecutableKind::Reducer, "effects-overwrite", '3'),
                            [](const json&, const json& incoming) { return json(incoming); });
        return std::move(builder).build();
    }

    static AdmissionProfile make_profile(const RegistrySnapshot& registry) {
        AdmissionProfileBuilder profile;
        profile.id("effects-profile")
            .semantic_version("1.0.0")
            .registry(registry)
            .mode(AdmissionMode::MultiTenant)
            .max_program_schema_version(PROGRAM_SCHEMA_VERSION_V4)
            .minimum_execution_guarantee(ExecutionGuarantee::Strict)
            .allow_source_kind(SourceKind::CppBuilder)
            .allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry.identities()) profile.allow_executable(identity);
        return std::move(profile).build();
    }

    static PolicySnapshot make_policy(const AdmissionProfile& profile) {
        PolicySnapshotBuilder policy;
        policy.id("effects-policy")
            .semantic_version("1.0.0")
            .owner_scope(kOwner)
            .admission_profile(profile)
            .budget_ceiling(BudgetLimits{10000, 1000, 1000, 4, 32, 20, 4, 4, 32})
            .minimum_execution_guarantee(ExecutionGuarantee::Strict);
        return std::move(policy).build();
    }

    fs::path root_;
    RegistrySnapshot registry_;
    AdmissionProfile profile_;
    PolicySnapshot policy_;
    std::shared_ptr<ToolExecutionController> controller_ =
        std::make_shared<ToolExecutionController>();
    std::unique_ptr<LedgerTool> tool_;
    std::unique_ptr<LedgerTool> foreign_tool_;
    std::shared_ptr<SQLiteToolEffectBroker> broker_;
    std::shared_ptr<SQLiteProgramStore> program_store_;
    std::shared_ptr<SQLiteProgramTransitionStore> transitions_;
    std::shared_ptr<graph::SqliteCheckpointStore> checkpoints_;
    std::shared_ptr<SQLiteProgramCoreToolGrantStore> grant_store_;
    std::shared_ptr<EngineGenerationCache> engines_;
    std::shared_ptr<ProgramCatalog> catalog_;
    std::optional<ProgramVersion> version_;
    std::unique_ptr<ProgramRuntime> runtime_;
    std::mutex mutex_;
    std::map<std::string, RunPlan> plans_;
    std::vector<NodeObservation> observations_;
    std::vector<NodeDispatch> dispatches_;
    std::atomic<std::uint64_t> next_generation_{0};
};

asio::awaitable<graph::NodeOutput> EffectBatchNode::run(graph::NodeInput in) {
    const std::string run = in.ctx.tool_execution_identity.root_run_id;
    const auto plan = host_->plan_for(run);
    ToolGateContext gate_context;
    gate_context.thread_id = in.ctx.thread_id;
    gate_context.step = in.ctx.step;
    auto execution = graph::make_tool_execution_context(in.ctx);
    if (plan.post_commit_stop) plan.post_commit_stop->bind(execution);
    host_->record_dispatch({run, execution.identity.thread_id, execution.effect_task_id,
                            execution.effect_grant.grant_id,
                            execution.effect_grant.binding_fingerprint,
                            execution.effect_grant.attempt, generation_});
    // GCC 13 cannot lower nested temporary vectors across this co_await.
    std::vector<ToolCall> calls = plan.calls;
    std::vector<Tool*> tools{host_->node_tool()};
    auto messages = co_await dispatch_tool_calls(std::move(calls), std::move(tools),
                                                 in.ctx.tool_gate, std::move(gate_context),
                                                 std::move(execution));
    json summary = json::array();
    for (const auto& message : messages)
        summary.push_back(json{{"id", message.tool_call_id},
                               {"status", message.tool_status},
                               {"uncertain", message.tool_effect_uncertain}});
    host_->observe({run, std::move(messages)});
    if (plan.interrupt_after && !in.ctx.resume_value)
        throw graph::NodeInterrupt("approval required", json{{"request", "approve"}});
    graph::NodeOutput output;
    output.writes.push_back(graph::ChannelWrite{"value", std::move(summary)});
    co_return output;
}

// ── Test helpers ─────────────────────────────────────────────────────────

// Every call carries the same model-supplied id: the durable slot must come
// from the call's position in the batch, never from the provider's id.
ToolCall ledger_call(json arguments, std::string model_id = "model-call") {
    return ToolCall{std::move(model_id), "ledger", arguments.dump()};
}

std::vector<ToolCall> batch(const std::string& run, int size) {
    std::vector<ToolCall> calls;
    for (int n = 0; n < size; ++n) calls.push_back(ledger_call(json{{"run", run}, {"n", n}}));
    return calls;
}

std::vector<std::string> statuses(const NodeObservation& observation) {
    std::vector<std::string> out;
    for (const auto& message : observation.messages) out.push_back(message.tool_status);
    return out;
}

std::vector<std::string> contents(const NodeObservation& observation) {
    std::vector<std::string> out;
    for (const auto& message : observation.messages) out.push_back(message.content);
    return out;
}

// Runs `run` until its approval interrupt, after a 3-call batch committed
// three external effects and three receipts.
ProgramResult first_attempt(EffectHost& host, const std::string& run, int calls = 3) {
    host.set_plan(run, RunPlan{batch(run, calls), true});
    const auto interrupted = host.start(run).wait();
    EXPECT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(interrupted.attempt(), 1U);
    return interrupted;
}

// Recreate all production execution state from the durable files. Only the
// test host's plans and observations survive as instrumentation.
ProgramResult recover(EffectHost& host, const std::string& run, OpenOptions options = {}) {
    host.open(std::move(options));
    const auto reconnected = host.runtime().reconnect(kOwner, run).wait();
    EXPECT_EQ(reconnected.status(), ProgramTerminalStatus::Interrupted);
    return reconnected;
}

void expect_same_rows(const std::vector<EffectRow>& before, const std::vector<EffectRow>& after) {
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        EXPECT_EQ(before[i].slot(), after[i].slot());
        EXPECT_EQ(before[i].version, after[i].version);
        EXPECT_EQ(before[i].operation, after[i].operation);
        EXPECT_EQ(before[i].fingerprint, after[i].fingerprint);
        EXPECT_EQ(before[i].tool_name, after[i].tool_name);
        EXPECT_EQ(before[i].grant, after[i].grant);
        EXPECT_EQ(before[i].executable, after[i].executable);
        EXPECT_EQ(before[i].arguments, after[i].arguments);
        EXPECT_EQ(before[i].first_attempt, after[i].first_attempt);
        EXPECT_EQ(before[i].has_receipt, after[i].has_receipt);
        EXPECT_EQ(before[i].receipt, after[i].receipt);
    }
}

// Cancellation and run-deadline expiry are terminal Program states, not
// approval interrupts. Exercise the real resume rejection with fresh attempt
// authority after rebuilding every durable store/runtime/Tool, rather than
// fabricating an attempt-2 execution context to bypass Program ownership.
void expect_terminal_marker_retained(EffectHost& host, const std::string& run,
                                     const ProgramResult& terminal,
                                     const std::vector<EffectRow>& original_rows,
                                     const std::vector<std::string>& original_ledger) {
    ASSERT_EQ(original_rows.size(), 1U);
    ASSERT_EQ(original_ledger.size(), 1U);
    EXPECT_FALSE(original_rows[0].has_receipt);
    expect_same_rows(original_rows, effect_rows(host.journal_path()));
    EXPECT_EQ(LedgerTool::payloads(host.ledger_path()), original_ledger);
    EXPECT_EQ(host.tool().executions.load(), 1);
    EXPECT_TRUE(host.observations(run).empty());
    const auto first_dispatches = host.dispatches(run);
    ASSERT_EQ(first_dispatches.size(), 1U);
    const auto& row = original_rows[0];
    EXPECT_EQ(row.owner, kOwner);
    EXPECT_EQ(row.run, run);
    EXPECT_EQ(row.version, terminal.program_version_id());
    EXPECT_EQ(row.operation, "root");
    EXPECT_EQ(row.thread, first_dispatches[0].thread);
    EXPECT_EQ(row.task, first_dispatches[0].task);
    EXPECT_FALSE(row.thread.empty());
    EXPECT_FALSE(row.task.empty());
    EXPECT_EQ(row.ordinal, 0);
    EXPECT_EQ(row.grant, first_dispatches[0].grant);
    EXPECT_EQ(row.grant, host.grant_for(run));
    EXPECT_EQ(row.fingerprint, first_dispatches[0].fingerprint);
    EXPECT_FALSE(row.fingerprint.empty());
    EXPECT_EQ(row.executable, kExecutable);
    EXPECT_EQ(row.tool_name, "ledger");
    EXPECT_EQ(json::parse(row.arguments), json::parse(original_ledger[0]));
    EXPECT_EQ(row.first_attempt, 1);
    EXPECT_EQ(first_dispatches[0].attempt, 1U);

    host.open();  // drops the stop hook and creates a genuinely new Tool/runtime
    EXPECT_EQ(host.version().id(), terminal.program_version_id());
    auto reconnected = host.runtime().reconnect(kOwner, run);
    const auto retained = reconnected.wait();
    EXPECT_EQ(retained.id(), terminal.id());
    EXPECT_EQ(retained.status(), terminal.status());
    EXPECT_EQ(retained.attempt(), 1U);
    const auto snapshot = reconnected.snapshot();
    EXPECT_EQ(snapshot.continuation().state,
              terminal.status() == ProgramTerminalStatus::Cancelled
                  ? ContinuationState::Cancelled : ContinuationState::TimedOut);
    EXPECT_EQ(snapshot.continuation().attempt, 1U);
    ASSERT_TRUE(snapshot.terminal_result());
    EXPECT_EQ(snapshot.terminal_result()->id(), terminal.id());
    EXPECT_EQ(host.admit_grant(run, 2), ProgramCoreToolGrantAdmission::Admitted);
    try {
        auto unexpected = host.runtime().resume(
            kOwner, run, ProgramResume{json::object(), "resume-terminal-trace", {}, {}});
        ADD_FAILURE() << "Terminal Program run unexpectedly accepted resume";
        // If ownership regresses, still drain any work before host destruction.
        (void)unexpected.wait();
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RESUME_STATE");
    }
    auto after_rejection = host.runtime().reconnect(kOwner, run);
    EXPECT_EQ(after_rejection.wait().id(), terminal.id());
    EXPECT_EQ(after_rejection.snapshot().id(), snapshot.id());
    EXPECT_EQ(after_rejection.snapshot().journal_head(), snapshot.journal_head());
    EXPECT_EQ(after_rejection.snapshot().event_sequence(), snapshot.event_sequence());
    EXPECT_EQ(after_rejection.attempt(), 1U);
    EXPECT_EQ(host.dispatches(run).size(), 1U);  // no new slot or node dispatch
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::payloads(host.ledger_path()), original_ledger);
    expect_same_rows(original_rows, effect_rows(host.journal_path()));
}

}  // namespace

TEST(ProgramToolEffects, ConcurrentRunsGetDistinctStableSlotsIndependentOfModelCallIds) {
    constexpr int kRuns = 4;
    constexpr int kBatch = 3;
    EffectHost host(std::make_shared<Rendezvous>(2));
    std::vector<std::string> runs;
    for (int r = 0; r < kRuns; ++r) {
        runs.push_back("run-" + std::to_string(r));
        auto calls = batch(runs.back(), kBatch);
        // Two distinct runs must be inside the Tool at the same time.
        auto first = json::parse(calls[0].arguments);
        first["rendezvous"] = runs.back();
        calls[0].arguments = first.dump();
        host.set_plan(runs.back(), RunPlan{std::move(calls), false});
    }

    std::vector<ProgramHandle> handles;
    for (const auto& run : runs) handles.push_back(host.start(run));
    for (auto& handle : handles)
        EXPECT_EQ(handle.wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_TRUE(host.rendezvous_->overlapped);

    EXPECT_EQ(LedgerTool::count(host.ledger_path()), static_cast<std::size_t>(kRuns * kBatch));
    EXPECT_EQ(host.tool().executions.load(), kRuns * kBatch);

    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kRuns * kBatch));
    std::set<std::tuple<std::string, std::string, std::string, std::string, std::int64_t>> slots;
    std::set<std::string> threads, tasks;
    std::map<std::string, std::set<std::int64_t>> ordinals;
    for (const auto& row : rows) {
        EXPECT_TRUE(slots.insert(row.slot()).second);
        threads.insert(row.thread);
        tasks.insert(row.task);
        ordinals[row.run].insert(row.ordinal);
        EXPECT_TRUE(row.has_receipt);
        EXPECT_EQ(row.first_attempt, 1);
        EXPECT_EQ(row.grant, host.grant_for(row.run));
        EXPECT_EQ(row.version, host.version().id());
        EXPECT_EQ(row.operation, "root");
        EXPECT_EQ(row.executable, kExecutable);
        EXPECT_EQ(row.tool_name, "ledger");
        const auto dispatches = host.dispatches(row.run);
        ASSERT_EQ(dispatches.size(), 1U);
        EXPECT_EQ(row.owner, kOwner);
        EXPECT_EQ(row.thread, dispatches[0].thread);
        EXPECT_EQ(row.task, dispatches[0].task);
        EXPECT_EQ(row.fingerprint, dispatches[0].fingerprint);
        EXPECT_FALSE(row.task.empty());
        EXPECT_FALSE(row.fingerprint.empty());
        // The slot is the call's batch position: ordinal n carries argument n.
        const auto arguments = json::parse(row.arguments);
        EXPECT_EQ(arguments.at("run"), row.run);
        EXPECT_EQ(arguments.at("n"), row.ordinal);
    }
    EXPECT_EQ(threads.size(), static_cast<std::size_t>(kRuns));
    EXPECT_EQ(tasks.size(), 1U);
    EXPECT_FALSE(tasks.begin()->empty());
    ASSERT_EQ(ordinals.size(), static_cast<std::size_t>(kRuns));
    for (const auto& [run, seen] : ordinals)
        EXPECT_EQ(seen, (std::set<std::int64_t>{0, 1, 2})) << run;

    // Every call of every batch reached the dispatcher with one duplicated model id.
    for (const auto& run : runs) {
        const auto observed = host.observations(run);
        ASSERT_EQ(observed.size(), 1U);
        ASSERT_EQ(observed[0].messages.size(), static_cast<std::size_t>(kBatch));
        for (const auto& message : observed[0].messages) {
            EXPECT_EQ(message.tool_call_id, "model-call");
            EXPECT_EQ(message.tool_status, "succeeded");
        }
    }
}

TEST(ProgramToolEffects, ReconnectAndAttemptAdvancementReplayWithoutExecutingAnyTool) {
    EffectHost host;
    const auto interrupted = first_attempt(host, "run-replay");
    const auto rows_before = effect_rows(host.journal_path());
    const auto ledger_before = LedgerTool::payloads(host.ledger_path());
    ASSERT_EQ(rows_before.size(), 3U);
    ASSERT_EQ(ledger_before.size(), 3U);
    for (const auto& row : rows_before) EXPECT_TRUE(row.has_receipt);
    const auto first_outputs = contents(host.observations("run-replay").at(0));

    const auto reconnected = recover(host, "run-replay");
    EXPECT_EQ(host.tool().executions.load(), 0);
    auto changed_ids = batch("run-replay", 3);
    for (auto& call : changed_ids) call.id = "changed-model-id";
    host.set_plan("run-replay", RunPlan{std::move(changed_ids), true});
    const auto resumed = host.resume(reconnected);
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(resumed.attempt(), 2U);
    EXPECT_EQ(interrupted.attempt(), 1U);

    // The node ran again under attempt 2 and was served from the receipts.
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::payloads(host.ledger_path()), ledger_before);
    expect_same_rows(rows_before, effect_rows(host.journal_path()));
    const auto observed = host.observations("run-replay");
    ASSERT_EQ(observed.size(), 2U);
    EXPECT_EQ(statuses(observed[1]), (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(contents(observed[1]), first_outputs);
    const auto dispatches = host.dispatches("run-replay");
    ASSERT_EQ(dispatches.size(), 2U);
    EXPECT_NE(dispatches[0].generation, dispatches[1].generation);
    EXPECT_EQ(dispatches[0].thread, dispatches[1].thread);
    EXPECT_EQ(dispatches[0].task, dispatches[1].task);
    EXPECT_EQ(dispatches[0].grant, dispatches[1].grant);
    EXPECT_EQ(dispatches[0].fingerprint, dispatches[1].fingerprint);
    EXPECT_EQ(dispatches[0].attempt, 1U);
    EXPECT_EQ(dispatches[1].attempt, 2U);
    for (const auto& row : rows_before) {
        EXPECT_EQ(row.thread, dispatches[1].thread);
        EXPECT_EQ(row.task, dispatches[1].task);
    }
    for (const auto& message : observed[1].messages)
        EXPECT_EQ(message.tool_call_id, "changed-model-id");
}

TEST(ProgramToolEffects, RecoveryWithoutFreshAttemptAuthorityRejectsEvenCommittedReplay) {
    EffectHost host;
    first_attempt(host, "run-no-fresh-grant");
    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), 3U);
    const auto reconnected = recover(host, "run-no-fresh-grant");
    const auto resumed = host.resume(reconnected, {}, false);
    EXPECT_EQ(resumed.attempt(), 2U);
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    const auto observed = host.observations("run-no-fresh-grant");
    ASSERT_EQ(observed.size(), 2U);
    EXPECT_EQ(statuses(observed[1]), (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 3U);
    expect_same_rows(rows, effect_rows(host.journal_path()));
}

namespace {
struct MismatchOutcome {
    std::vector<std::string> statuses;
    std::size_t ledger = 0;
    int executions = 0;
    std::vector<EffectRow> rows_before, rows_after;
};

// First attempt commits `initial` calls; recovery runs attempt 2 with `mutate`
// applied to the plan, the host reopened with `options` and `grant_id`.
MismatchOutcome recover_with_mismatch(const std::string& run, int initial, OpenOptions options,
                                      std::function<void(std::vector<ToolCall>&)> mutate,
                                      std::string grant_id = {}) {
    EffectHost host;
    first_attempt(host, run, initial);
    MismatchOutcome outcome;
    outcome.rows_before = effect_rows(host.journal_path());
    const auto reconnected = recover(host, run, options);
    auto calls = batch(run, initial);
    if (mutate) mutate(calls);
    host.set_plan(run, RunPlan{std::move(calls), true});
    const auto resumed = host.resume(reconnected, std::move(grant_id));
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed);
    outcome.statuses = statuses(host.observations(run).at(1));
    outcome.ledger = LedgerTool::count(host.ledger_path());
    outcome.executions = host.tool().executions.load();
    outcome.rows_after = effect_rows(host.journal_path());
    return outcome;
}
}  // namespace

TEST(ProgramToolEffects, RecoveryWithChangedGrantAuthorityFailsClosed) {
    const auto outcome = recover_with_mismatch("run-grant", 3, {}, {}, "substituted-grant");
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(ProgramToolEffects, RecoveryWithChangedExecutableIdentityFailsClosed) {
    OpenOptions options;
    options.executable = "ledger-build:v2";
    const auto outcome = recover_with_mismatch("run-executable", 3, options, {});
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(ProgramToolEffects, RecoveryWithHostToolNotBoundToBrokerFailsClosed) {
    OpenOptions options;
    options.bind_foreign_tool = true;
    const auto outcome = recover_with_mismatch("run-unbound", 3, options, {});
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "rejected")));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(ProgramToolEffects, RecoveryWithChangedArgumentsRejectsOnlyTheChangedSlot) {
    const auto outcome = recover_with_mismatch("run-arguments", 3, {}, [](auto& calls) {
        calls[1] = ledger_call(json{{"run", "run-arguments"}, {"n", 1}, {"extra", true}});
    });
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>{"succeeded", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(ProgramToolEffects, RecoveryWithReorderedBatchRejectsBothMovedSlots) {
    const auto outcome = recover_with_mismatch("run-reorder", 3, {}, [](auto& calls) {
        std::swap(calls[0], calls[1]);
    });
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>{"rejected", "rejected", "succeeded"}));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 0);
    expect_same_rows(outcome.rows_before, outcome.rows_after);
}

TEST(ProgramToolEffects, RecoveryReplaysCommittedSlotsAndOnlyANewOrdinalExecutes) {
    const auto outcome = recover_with_mismatch("run-grown", 2, {}, [](auto& calls) {
        calls.push_back(ledger_call(json{{"run", "run-grown"}, {"n", 2}}));
    });
    EXPECT_EQ(outcome.statuses, (std::vector<std::string>(3, "succeeded")));
    EXPECT_EQ(outcome.ledger, 3U);
    EXPECT_EQ(outcome.executions, 1);
    ASSERT_EQ(outcome.rows_after.size(), 3U);
    EXPECT_EQ(outcome.rows_after[2].ordinal, 2);
    EXPECT_EQ(outcome.rows_after[2].first_attempt, 2);
    ASSERT_EQ(outcome.rows_before.size(), 2U);
    const std::vector<EffectRow> committed(outcome.rows_after.begin(),
                                           outcome.rows_after.begin() + 2);
    expect_same_rows(outcome.rows_before, committed);
}

TEST(ProgramToolEffects, MarkerCommitFailureExecutesNoTool) {
    EffectHost host;
    Db(host.journal_path()).exec(
        "CREATE TRIGGER deny_marker BEFORE INSERT ON neograph_tool_effects "
        "BEGIN SELECT RAISE(ABORT, 'marker denied'); END");
    host.set_plan("run-marker", RunPlan{batch("run-marker", 2), false});
    EXPECT_EQ(host.start("run-marker").wait().status(), ProgramTerminalStatus::Completed);
    const auto observed = host.observations("run-marker");
    ASSERT_EQ(observed.size(), 1U);
    ASSERT_EQ(observed[0].messages.size(), 2U);
    for (const auto& message : observed[0].messages) {
        EXPECT_EQ(message.tool_status, "failed");
        EXPECT_FALSE(message.tool_effect_uncertain);
        EXPECT_NE(message.content.find("marker not confirmed"), std::string::npos);
    }
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 0U);
    EXPECT_TRUE(effect_rows(host.journal_path()).empty());

    // The refusal came from the injected fault: with it gone a new run proceeds.
    Db(host.journal_path()).exec("DROP TRIGGER deny_marker");
    host.set_plan("run-after", RunPlan{batch("run-after", 2), false});
    EXPECT_EQ(host.start("run-after").wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
    EXPECT_EQ(effect_rows(host.journal_path()).size(), 2U);
}

TEST(ProgramToolEffects, ReceiptCommitFailureBlocksAndIsNeverReExecuted) {
    EffectHost host;
    Db(host.journal_path()).exec(
        "CREATE TRIGGER deny_receipt BEFORE UPDATE ON neograph_tool_effects "
        "BEGIN SELECT RAISE(ABORT, 'receipt denied'); END");
    host.set_plan("run-receipt", RunPlan{batch("run-receipt", 2), false});
    const auto blocked = host.start("run-receipt").wait();
    EXPECT_EQ(blocked.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
    auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), 2U);
    for (const auto& row : rows) EXPECT_FALSE(row.has_receipt);
    EXPECT_TRUE(host.observations("run-receipt").empty());  // dispatch never returned

    Db(host.journal_path()).exec("DROP TRIGGER deny_receipt");
    auto current = recover(host, "run-receipt");
    for (int resume = 0; resume < 2; ++resume) {
        current = host.resume(current);
        EXPECT_EQ(current.status(), ProgramTerminalStatus::Interrupted);
        EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
        EXPECT_EQ(host.tool().executions.load(), 0);
    }
    rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), 2U);
    for (const auto& row : rows) EXPECT_FALSE(row.has_receipt);
}

TEST(ProgramToolEffects, LostResponseAfterDispatchStaysUnresolvedAcrossRestart) {
    EffectHost host;
    std::vector<ToolCall> calls{ledger_call(json{{"run", "run-lost"}, {"n", 0}}),
                                ledger_call(json{{"run", "run-lost"}, {"n", 1}, {"lose", true}})};
    host.set_plan("run-lost", RunPlan{calls, false});
    const auto blocked = host.start("run-lost").wait();
    EXPECT_EQ(blocked.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
    const auto rows_before = effect_rows(host.journal_path());
    ASSERT_EQ(rows_before.size(), 2U);
    EXPECT_TRUE(rows_before[0].has_receipt);
    EXPECT_FALSE(rows_before[1].has_receipt);

    const auto reconnected = recover(host, "run-lost");
    const auto resumed = host.resume(reconnected);
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
    expect_same_rows(rows_before, effect_rows(host.journal_path()));
}

TEST(ProgramToolEffects, GateDenialDispatchesNothingAndKeepsLaterSlotsStable) {
    EffectHost host;
    std::vector<ToolCall> calls{ledger_call(json{{"run", "run-deny"}, {"n", 0}, {"deny", true}}),
                                ledger_call(json{{"run", "run-deny"}, {"n", 1}})};
    host.set_plan("run-deny", RunPlan{calls, false});
    EXPECT_EQ(host.start("run-deny").wait().status(), ProgramTerminalStatus::Completed);
    const auto observed = host.observations("run-deny");
    ASSERT_EQ(observed.size(), 1U);
    EXPECT_EQ(statuses(observed[0]), (std::vector<std::string>{"rejected", "succeeded"}));
    EXPECT_EQ(host.tool().executions.load(), 1);
    const auto ledger = LedgerTool::payloads(host.ledger_path());
    ASSERT_EQ(ledger.size(), 1U);
    EXPECT_EQ(json::parse(ledger[0]).at("n"), 1);
    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), 1U);  // the denied call never reserved a slot
    EXPECT_EQ(rows[0].ordinal, 1);
}

TEST(ProgramToolEffects, EquivalentCapabilitiesAndArgumentsInAnotherRunCannotReplayFirstRun) {
    EffectHost host;
    const auto calls = batch("same-payload", 2);
    host.set_plan("isolation-first", RunPlan{calls, true});
    EXPECT_EQ(host.start("isolation-first", "trace", "first-grant").wait().status(),
              ProgramTerminalStatus::Interrupted);
    const auto first_rows = effect_rows(host.journal_path());
    ASSERT_EQ(first_rows.size(), 2U);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 2U);
    const auto reconnected = recover(host, "isolation-first");
    host.set_plan("isolation-second", RunPlan{calls, false});
    EXPECT_EQ(host.start("isolation-second", "trace", "second-grant").wait().status(),
              ProgramTerminalStatus::Completed);
    EXPECT_EQ(host.tool().executions.load(), 2);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 4U);
    EXPECT_EQ(host.resume(reconnected, "first-grant").status(),
              ProgramTerminalStatus::Completed);
    EXPECT_EQ(host.tool().executions.load(), 2);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 4U);
    const auto all_rows = effect_rows(host.journal_path());
    ASSERT_EQ(all_rows.size(), 4U);
    const auto first_dispatches = host.dispatches("isolation-first");
    const auto second_dispatches = host.dispatches("isolation-second");
    ASSERT_EQ(first_dispatches.size(), 2U);
    ASSERT_EQ(second_dispatches.size(), 1U);
    EXPECT_EQ(first_dispatches[0].thread, first_dispatches[1].thread);
    EXPECT_NE(first_dispatches[1].thread, second_dispatches[0].thread);
    for (std::size_t i = 0; i < first_rows.size(); ++i) {
        EXPECT_EQ(first_rows[i].arguments, all_rows[i + 2].arguments);
        EXPECT_NE(first_rows[i].grant, all_rows[i + 2].grant);
        EXPECT_NE(first_rows[i].slot(), all_rows[i + 2].slot());
    }
    const std::vector<EffectRow> retained(all_rows.begin(), all_rows.begin() + 2);
    expect_same_rows(first_rows, retained);
}

class ProgramToolEffectStoredBinding : public ::testing::TestWithParam<std::string> {};

TEST_P(ProgramToolEffectStoredBinding, ExactOriginalRuntimeSlotRejectsAlteredPersistedBinding) {
    EffectHost host;
    first_attempt(host, "run-stored-binding");
    const auto original = effect_rows(host.journal_path());
    ASSERT_EQ(original.size(), 3U);
    host.close();
    update_effect_field(host.journal_path(), original[1], GetParam(), "substituted-binding");
    const auto damaged = effect_rows(host.journal_path());
    const auto reconnected = recover(host, "run-stored-binding");
    EXPECT_EQ(host.resume(reconnected).status(), ProgramTerminalStatus::Completed);
    const auto observed = host.observations("run-stored-binding");
    ASSERT_EQ(observed.size(), 2U);
    EXPECT_EQ(statuses(observed[1]),
              (std::vector<std::string>{"succeeded", "rejected", "succeeded"}));
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 3U);
    expect_same_rows(damaged, effect_rows(host.journal_path()));
}

INSTANTIATE_TEST_SUITE_P(
    DurableAuthority, ProgramToolEffectStoredBinding,
    ::testing::Values("program_version", "operation", "grant_id", "binding_fingerprint",
                      "executable", "tool_name", "arguments"));

TEST(ProgramToolEffects, CorruptReceiptAtExactOriginalRuntimeSlotBlocksWithoutExecuting) {
    EffectHost host;
    first_attempt(host, "run-corrupt");
    const auto original = effect_rows(host.journal_path());
    ASSERT_EQ(original.size(), 3U);
    host.close();
    update_effect_field(host.journal_path(), original[1], "receipt", "{not-json");
    const auto damaged = effect_rows(host.journal_path());
    auto current = recover(host, "run-corrupt");
    for (int resume = 0; resume < 2; ++resume) {
        current = host.resume(current);
        EXPECT_EQ(current.status(), ProgramTerminalStatus::Interrupted);
        EXPECT_EQ(host.tool().executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(host.ledger_path()), 3U);
    }
    expect_same_rows(damaged, effect_rows(host.journal_path()));
}

TEST(ProgramToolEffects, ActualMarkerCommitSyncFailureDispatchesNoTool) {
    ScopedWalSyncFault fault;
    EffectHost host;
    host.close();
    fault.target(host.journal_path());
    host.open();
    fault.enable();
    host.set_plan("run-marker-sync", RunPlan{batch("run-marker-sync", 1), false});
    EXPECT_EQ(host.start("run-marker-sync").wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_GT(fault.failures(), 0U);  // the actual WAL COMMIT xSync was exercised
    EXPECT_EQ(host.tool().executions.load(), 0);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 0U);
    const auto observed = host.observations("run-marker-sync");
    ASSERT_EQ(observed.size(), 1U);
    EXPECT_EQ(statuses(observed[0]), (std::vector<std::string>{"failed"}));
    // A COMMIT I/O failure is potentially ambiguous; it must never dispatch.
    // Any marker surviving recovery remains unresolved, not a fabricated receipt.
    fault.disable();
    host.close();
    for (const auto& row : effect_rows(host.journal_path())) EXPECT_FALSE(row.has_receipt);
    host.open();
    host.set_plan("run-marker-repaired", RunPlan{batch("run-marker-repaired", 1), false});
    EXPECT_EQ(host.start("run-marker-repaired").wait().status(), ProgramTerminalStatus::Completed);
    EXPECT_EQ(host.tool().executions.load(), 1);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 1U);
}

TEST(ProgramToolEffects, ActualReceiptCommitSyncFailureNeverReexecutesAfterRestart) {
    ScopedWalSyncFault fault;
    EffectHost host;
    host.close();
    fault.target(host.journal_path());
    OpenOptions options;
    options.after_effect = [&fault] { fault.enable(); };
    host.open(std::move(options));
    host.set_plan("run-receipt-sync", RunPlan{batch("run-receipt-sync", 1), false});
    const auto interrupted = host.start("run-receipt-sync").wait();
    EXPECT_EQ(interrupted.status(), ProgramTerminalStatus::Interrupted);
    EXPECT_GT(fault.failures(), 0U);  // marker succeeded; receipt COMMIT failed
    EXPECT_EQ(host.tool().executions.load(), 1);
    EXPECT_EQ(LedgerTool::count(host.ledger_path()), 1U);
    fault.disable();
    auto current = recover(host, "run-receipt-sync");
    const auto recovered_rows = effect_rows(host.journal_path());
    ASSERT_EQ(recovered_rows.size(), 1U);
    for (int resume = 0; resume < 2; ++resume) {
        current = host.resume(current);
        // An ambiguous fsync failure may or may not leave a valid durable
        // receipt. Only a verified receipt permits completion; a pending marker
        // must still interrupt. Neither branch may execute the Tool again.
        EXPECT_EQ(current.status(), recovered_rows[0].has_receipt
                                        ? ProgramTerminalStatus::Completed
                                        : ProgramTerminalStatus::Interrupted);
        EXPECT_EQ(host.tool().executions.load(), 0);
        EXPECT_EQ(LedgerTool::count(host.ledger_path()), 1U);
        if (current.status() == ProgramTerminalStatus::Completed) break;
    }
    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].first_attempt, 1);
    expect_same_rows(recovered_rows, rows);
}

TEST(ProgramToolEffects, CancellationAfterExternalCommitRetainsExactMarkerAndRejectsResume) {
    auto fence = std::make_shared<PostCommitStopFence>();
    EffectHost host;
    OpenOptions options;
    options.after_effect = [fence] { fence->after_effect(); };
    host.open(std::move(options));
    const std::string run = "run-post-commit-cancel";
    host.set_plan(run, RunPlan{batch(run, 1), false, fence});
    auto handle = host.start(run);
    PostCommitStopRelease release{fence};
    ASSERT_TRUE(fence->wait_until_committed());
    // Independent SQLite reads establish a real external commit and an
    // unresolved runtime-generated marker before requesting cancellation.
    const auto ledger = LedgerTool::payloads(host.ledger_path());
    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(ledger.size(), 1U);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_FALSE(rows[0].has_receipt);
    EXPECT_FALSE(handle.try_result());
    ASSERT_TRUE(handle.cancel());
    const auto terminal = handle.wait();  // drains the blocking Tool/controller
    EXPECT_EQ(terminal.status(), ProgramTerminalStatus::Cancelled);
    EXPECT_EQ(terminal.attempt(), 1U);
    ASSERT_TRUE(terminal.failure());
    EXPECT_EQ(terminal.failure()->code, "P_RUNTIME_CANCELLED");
    {
        std::lock_guard lock(fence->mutex);
        EXPECT_TRUE(fence->stopped);
        EXPECT_FALSE(fence->stopped_before_commit);
        EXPECT_LT(fence->committed_at, *fence->deadline);
        EXPECT_GE(fence->stopped_at, fence->committed_at);
    }
    expect_terminal_marker_retained(host, run, terminal, rows, ledger);
}

TEST(ProgramToolEffects, RunDeadlineAfterExternalCommitRetainsExactMarkerAndRejectsResume) {
    auto fence = std::make_shared<PostCommitStopFence>();
    EffectHost host;
    OpenOptions options;
    options.after_effect = [fence] { fence->after_effect(); };
    host.open(std::move(options));
    const std::string run = "run-post-commit-deadline";
    host.set_plan(run, RunPlan{batch(run, 1), false, fence});
    auto handle = host.start(run);
    PostCommitStopRelease release{fence};
    ASSERT_TRUE(fence->wait_until_committed());
    const auto ledger = LedgerTool::payloads(host.ledger_path());
    const auto rows = effect_rows(host.journal_path());
    ASSERT_EQ(ledger.size(), 1U);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_FALSE(rows[0].has_receipt);
    // Keep the existing admitted wall-time budget unchanged. Only Program's
    // real run deadline cancels the execution token and releases this fence;
    // no test timer, timeout repinning, manual cancel, or synthetic loss.
    const auto terminal = handle.wait();
    EXPECT_EQ(terminal.status(), ProgramTerminalStatus::TimedOut);
    EXPECT_EQ(terminal.attempt(), 1U);
    ASSERT_TRUE(terminal.failure());
    EXPECT_EQ(terminal.failure()->code, "P_RUNTIME_TIMEOUT");
    {
        std::lock_guard lock(fence->mutex);
        EXPECT_TRUE(fence->stopped);
        EXPECT_FALSE(fence->stopped_before_commit);
        EXPECT_LT(fence->committed_at, *fence->deadline);
        EXPECT_GE(fence->stopped_at, *fence->deadline);
    }
    expect_terminal_marker_retained(host, run, terminal, rows, ledger);
}
