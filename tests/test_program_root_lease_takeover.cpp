// Recovery of a running Core-only root that holds an execution lease.
//
// A define()-only Program started without child budgets holds an execution lease while its root Core call runs.
// reconnect() of such a run takes it over by the ownership fence, as it takes over every other run, and deletes the
// lease in the same step; the recovered attempt runs without a lease and only until the run's absolute wall deadline.
// A process that still runs the run after the takeover learns of it before its next Core checkpoint save and stops.
//
// The Core here is a Send fan-out between two plain steps: "head" runs first, "fan" sends three invocations of
// "work", each of which appends its value to "items", then "tail" and "last" run one after the other. Every node body
// records that it ran in the world of its run.
#include "program_resume_fixture.h"

#include "../src/core/runtime_clock.h"
#include "fixtures/relation_forwarding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <future>

namespace {
using namespace neograph;
using namespace neograph::program;

const std::array<std::string, 3> kSends{"left", "middle", "right"};
constexpr std::uint64_t          kWallMs = 60'000;

// ---------------------------------------------------------------------------
// Gates and the world of a run
// ---------------------------------------------------------------------------

/// Holds the first call of pass() after arm() until release(); every other call passes.
class Gate {
public:
    void arm() {
        std::lock_guard lock(mutex_);
        armed_    = true;
        entered_  = false;
        released_ = false;
    }
    void pass() {
        std::unique_lock lock(mutex_);
        if (!armed_) return;
        armed_   = false;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }
    void release() {
        std::lock_guard lock(mutex_);
        armed_    = false;
        released_ = true;
        changed_.notify_all();
    }
    bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }
    /// Waits until the gate is entered or `done` holds, and says whether it was entered.
    template <class Done>
    bool wait_entered_or(Done done) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return entered_ || done(); });
        return entered_;
    }
    /// Wakes a waiter of wait_entered_or after `done` changed.
    void notify() {
        std::lock_guard lock(mutex_);
        changed_.notify_all();
    }

private:
    mutable std::mutex      mutex_;
    std::condition_variable changed_;
    bool                    armed_ = false, entered_ = false, released_ = false;
};

/// Runs `release` when the scope ends, so that a test that stops early leaves nothing waiting.
struct ReleaseOnExit {
    std::function<void()> release;
    ~ReleaseOnExit() { release(); }
};

/// A world of the fixture (crash points and the journal of node bodies) with the gates of this file's nodes.
struct LeaseWorld final : World {
    explicit LeaseWorld(std::string name) : World(std::move(name)) {}
    // A test that stops early leaves no node body waiting.
    ~LeaseWorld() {
        head_gate.release();
        fan_gate.release();
        work_gate.release();
        tail_gate.release();
        last_gate.release();
    }
    Gate        head_gate, fan_gate, work_gate, tail_gate, last_gate;
    /// The head step fails once it passed its gate.
    std::atomic<bool> fail_head{false};
    std::string gated_work;  // the work value work_gate holds
    /// Runs in the fan node's body, after it is recorded.
    std::function<void()> on_fan;
};

LeaseWorld& lease_world_of(std::string_view run_id) { return static_cast<LeaseWorld&>(world_of(run_id)); }

std::size_t runs_of(const std::vector<StepJournal::Run>& runs, const std::string& value) {
    return static_cast<std::size_t>(
        std::count_if(runs.begin(), runs.end(), [&](const auto& run) { return run.value == value; }));
}

std::vector<StepJournal::Run> take_runs(World& world) {
    std::lock_guard lock(world.journal.mutex);
    auto runs = std::move(world.journal.runs);
    world.journal.runs.clear();
    return runs;
}

// ---------------------------------------------------------------------------
// The nodes
// ---------------------------------------------------------------------------

class FanNode final : public graph::GraphNode {
public:
    explicit FanNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        auto& world = lease_world_of(in.ctx.run_id);
        world.journal.record({in.ctx.thread_id, {}, "fan", {}});
        if (world.on_fan) world.on_fan();
        world.fan_gate.pass();
        graph::NodeOutput output;
        for (const auto& value : kSends) output.sends.push_back(graph::Send{"work", json{{"value", value}}});
        co_return output;
    }

private:
    std::string name_;
};

class WorkNode final : public graph::GraphNode {
public:
    explicit WorkNode(std::string name) : name_(std::move(name)) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        auto&       world = lease_world_of(in.ctx.run_id);
        const auto value = in.state.get("value").get<std::string>();
        world.journal.record({in.ctx.thread_id, {}, "work:" + value, {}});
        if (value == world.gated_work) world.work_gate.pass();
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{"items", json::array({value})});
        co_return output;
    }

private:
    std::string name_;
};

/// A plain step: records `label`, passes `gate`, fails when `fail` is set, and sets the boolean channel `channel`.
class PlainStep final : public graph::GraphNode {
public:
    PlainStep(std::string name, std::string label, Gate LeaseWorld::*gate, std::string channel,
              std::atomic<bool> LeaseWorld::*fail = nullptr)
        : name_(std::move(name)), label_(std::move(label)), gate_(gate), channel_(std::move(channel)), fail_(fail) {}
    std::string get_name() const override { return name_; }
    asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
        auto& world = lease_world_of(in.ctx.run_id);
        world.journal.record({in.ctx.thread_id, {}, label_, {}});
        if (gate_) (world.*gate_).pass();
        if (fail_ && (world.*fail_).load()) throw std::runtime_error("step " + label_ + " failed");
        graph::NodeOutput output;
        output.writes.push_back(graph::ChannelWrite{channel_, json(true)});
        co_return output;
    }

private:
    std::string                    name_, label_;
    Gate LeaseWorld::*             gate_;
    std::string                    channel_;
    std::atomic<bool> LeaseWorld::*fail_;
};

// ---------------------------------------------------------------------------
// Stores of one process
// ---------------------------------------------------------------------------

/// One process's view of a transition store that several processes share. Separate views are separate processes to
/// the runtime. The hooks let a test stop a process at a publication or fail its reads.
class ProcessView final : public SyncProgramTransitionStore {
public:
    NG_FORWARD_RELATION_BLOCKING(*inner_)
    explicit ProcessView(std::shared_ptr<ProgramTransitionStore> inner)
        : inner_(std::make_shared<BlockingProgramTransitionStore>(std::move(inner))) {}

    /// Runs before and after the safe point publication reaches the shared store.
    std::function<void()> before_safe_point, after_safe_point;
    /// Runs when a lineage is read by run id.
    std::function<void()> on_run_lineage_read;
    /// While set, reading a run record fails.
    std::atomic<bool> fail_run_reads{false};

    std::optional<ProgramRunRecord> load_sync(std::string_view owner, std::string_view run) const override {
        if (fail_run_reads.load()) throw std::runtime_error("injected run record read failure");
        return inner_->load(owner, run);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner, std::string_view run) const override {
        return inner_->latest(owner, run);
    }
    std::optional<ProgramRunPublicationHead> load_run_publication_head_sync(std::string_view owner,
                                                                            std::string_view run) const override {
        return inner_->load_run_publication_head(owner, run);
    }
    std::vector<ProgramEvent> load_events_sync(std::string_view owner, std::string_view run,
                                               std::uint64_t after) const override {
        return inner_->load_events(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(std::string_view owner, std::string_view run,
                                                            std::uint64_t after) const override {
        return inner_->load_effects(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        return inner_->load_javascript_commands(owner, run, after);
    }
    std::optional<ProgramCommandPublicationHead> load_command_publication_head_sync(
        std::string_view owner, std::string_view run) const override {
        return inner_->load_command_publication_head(owner, run);
    }
    std::vector<ProgramContextPublication> load_context_publications_sync(std::string_view owner,
                                                                          std::string_view run,
                                                                          std::uint64_t after) const override {
        return inner_->load_context_publications(owner, run, after);
    }
    std::vector<HookOutboxEntry> load_hook_outbox_entries_sync(std::string_view owner,
                                                               std::string_view run) const override {
        return inner_->load_hook_outbox_entries(owner, run);
    }
    void verify_stored_history_sync(std::string_view owner, std::string_view run) const override {
        inner_->verify_stored_history(owner, run);
    }
    ProgramEffectiveRuntimeState load_effective_runtime_state_sync(std::string_view owner,
                                                                   std::string_view run) const override {
        return inner_->load_effective_runtime_state(owner, run);
    }
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses_sync(std::string_view owner,
                                                                       std::string_view run) const override {
        return inner_->load_child_syntheses(owner, run);
    }
    std::optional<MigrationPlan> load_migration_plan_sync(std::string_view owner, std::string_view run) const override {
        return inner_->load_migration_plan(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner,
                                                       std::string_view lineage) const override {
        return inner_->load_lineage(owner, lineage);
    }
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner,
                                                           std::string_view run) const override {
        auto lineage = inner_->load_run_lineage(owner, run);
        if (on_run_lineage_read) on_run_lineage_read();
        return lineage;
    }
    std::optional<ProgramRunLineage> load_lineage_head_sync(std::string_view owner, std::string_view lineage,
                                                            std::string_view head) const override {
        return inner_->load_lineage_head(owner, lineage, head);
    }
    std::optional<ProgramRunGeneration> load_generation_sync(std::string_view owner, std::string_view lineage,
                                                             std::uint64_t generation) const override {
        return inner_->load_generation(owner, lineage, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner, std::string_view lineage, std::uint64_t generation) const override {
        return inner_->load_generation_initial_publication(owner, lineage, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(std::string_view owner,
                                                                           std::string_view source,
                                                                           std::string_view head) const override {
        return inner_->load_graph_migration_capsule(owner, source, head);
    }
    std::optional<ProgramExecutionLease> load_execution_lease_sync(std::string_view owner,
                                                                   std::string_view run) const override {
        return inner_->load_execution_lease(owner, run);
    }
    ProgramTransitionPublishResult compare_publish_sync(std::string_view owner, std::string_view head,
                                                        std::uint64_t expected_fence,
                                                        ProgramTransitionPublication publication) override {
        return inner_->compare_publish(owner, head, expected_fence, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(std::string_view owner, std::string_view head,
                                                                  std::uint64_t expected_fence,
                                                                  ProgramTransitionPublication publication,
                                                                  std::optional<ProgramExecutionLease> expected,
                                                                  std::optional<ProgramExecutionLease> next) override {
        return inner_->compare_publish_execution(owner, head, expected_fence, std::move(publication),
                                                 std::move(expected), std::move(next));
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner, std::string_view head, std::uint64_t expected_fence,
        ProgramTransitionPublication publication, const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule, const ProgramExecutionLease& lease) override {
        if (before_safe_point) before_safe_point();
        const auto published = inner_->compare_publish_graph_safe_point(owner, head, expected_fence,
                                                                        std::move(publication), evidence, capsule,
                                                                        lease);
        if (after_safe_point) after_safe_point();
        return published;
    }
    ProgramTransitionPublishResult compare_publish_takeover_sync(std::string_view owner, std::string_view run_id,
                                                                 std::string_view head,
                                                                 std::uint64_t expected_fence) override {
        return inner_->compare_publish_takeover(owner, run_id, head, expected_fence);
    }
    ProgramTransitionPublishResult compare_publish_takeover_execution_sync(
        std::string_view owner, std::string_view run_id, std::string_view head, std::uint64_t expected_fence,
        ProgramExecutionLease expected_lease) override {
        return inner_->compare_publish_takeover_execution(owner, run_id, head, expected_fence,
                                                          std::move(expected_lease));
    }

private:
    std::shared_ptr<BlockingProgramTransitionStore> inner_;
};

/// One process's view of a shared checkpoint store; counts the checkpoints the process saves, and can hold the first
/// save of a checkpoint that `hold_when` picks at `save_gate` before the save reaches the store. Every other
/// operation reaches the shared store unchanged.
class SaveCountingCheckpointStore final : public graph::CheckpointStore {
public:
    explicit SaveCountingCheckpointStore(std::shared_ptr<graph::CheckpointStore> inner) : inner_(std::move(inner)) {}
    std::atomic<int>                              saves{0};
    std::function<bool(const graph::Checkpoint&)> hold_when;
    Gate                                          save_gate;

    std::string saved_thread() const {
        std::lock_guard lock(mutex_);
        return saved_thread_;
    }

    void save(const graph::Checkpoint& checkpoint) override {
        hold(checkpoint);
        inner_->save(checkpoint);
        saved(checkpoint);
    }
    void publish_managed_budget_checkpoint(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                           const graph::Checkpoint& checkpoint) override {
        hold(checkpoint);
        inner_->publish_managed_budget_checkpoint(lease, checkpoint);
        saved(checkpoint);
    }
    void put_writes(const std::string& thread, const std::string& parent, const graph::PendingWrite& write) override {
        inner_->put_writes(thread, parent, write);
    }
    std::optional<graph::Checkpoint> load_latest(const std::string& thread) override {
        return inner_->load_latest(thread);
    }
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override { return inner_->load_by_id(id); }
    std::vector<graph::Checkpoint> list(const std::string& thread, int limit) override {
        return inner_->list(thread, limit);
    }
    void delete_thread(const std::string& thread) override { inner_->delete_thread(thread); }
    std::vector<graph::PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override {
        inner_->clear_writes(thread, parent);
    }
    bool requires_managed_budget(const std::string& thread) override {
        return inner_->requires_managed_budget(thread);
    }
    std::shared_ptr<graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const graph::ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override {
        return inner_->acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment);
    }
    graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override {
        return inner_->begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest);
    }
    void settle_managed_budget_effect(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                      const graph::ManagedBudgetEffectReceipt& effect,
                                      sp::runtime::Result genuine_outcome,
                                      const UsageAccumulator::AuthoritySnapshot& authority) override {
        inner_->settle_managed_budget_effect(lease, effect, std::move(genuine_outcome), authority);
    }
    void release_managed_budget_lease(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease) override {
        inner_->release_managed_budget_lease(lease);
    }
    bool retains_native_checkpoint() const noexcept override { return inner_->retains_native_checkpoint(); }
    void publish_managed_budget_fork(const graph::Checkpoint& source, const graph::Checkpoint& forked) override {
        inner_->publish_managed_budget_fork(source, forked);
    }

private:
    void hold(const graph::Checkpoint& checkpoint) {
        if (hold_when && hold_when(checkpoint)) save_gate.pass();
    }
    void saved(const graph::Checkpoint& checkpoint) {
        {
            std::lock_guard lock(mutex_);
            saved_thread_ = checkpoint.thread_id;
        }
        ++saves;
    }

    std::shared_ptr<graph::CheckpointStore> inner_;
    mutable std::mutex                      mutex_;
    std::string                             saved_thread_;
};

// ---------------------------------------------------------------------------
// One process: registry, catalog, runtime
// ---------------------------------------------------------------------------

ExecutableManifest lease_manifest(ExecutableIdentity identity) {
    return ExecutableManifest{std::move(identity), EffectMode::Brokered, "root-lease", {}, {}, {}};
}

RegistrySnapshot lease_registry() {
    RegistrySnapshotBuilder builder;
    builder.add_node(lease_manifest({ExecutableKind::Node, "lt.fan", "1.0.0", digest('a')}),
                     [](const std::string& name, const json&, const graph::NodeContext&) {
                         return std::make_unique<FanNode>(name);
                     },
                     json{{"type", "object"}}, json::object());
    builder.add_node(lease_manifest({ExecutableKind::Node, "lt.work", "1.0.0", digest('b')}),
                     [](const std::string& name, const json&, const graph::NodeContext&) {
                         return std::make_unique<WorkNode>(name);
                     },
                     json{{"type", "object"}}, json{{"writes", json::array({"items"})}});
    builder.add_node(lease_manifest({ExecutableKind::Node, "lt.head", "1.0.0", digest('f')}),
                     [](const std::string& name, const json&, const graph::NodeContext&) {
                         return std::make_unique<PlainStep>(name, "head", &LeaseWorld::head_gate, "started",
                                                            &LeaseWorld::fail_head);
                     },
                     json{{"type", "object"}}, json{{"writes", json::array({"started"})}});
    builder.add_node(lease_manifest({ExecutableKind::Node, "lt.tail", "1.0.0", digest('c')}),
                     [](const std::string& name, const json&, const graph::NodeContext&) {
                         return std::make_unique<PlainStep>(name, "tail", &LeaseWorld::tail_gate, "done");
                     },
                     json{{"type", "object"}}, json{{"writes", json::array({"done"})}});
    builder.add_node(lease_manifest({ExecutableKind::Node, "lt.last", "1.0.0", digest('9')}),
                     [](const std::string& name, const json&, const graph::NodeContext&) {
                         return std::make_unique<PlainStep>(name, "last", &LeaseWorld::last_gate, "finished");
                     },
                     json{{"type", "object"}}, json{{"writes", json::array({"finished"})}});
    builder.add_reducer(lease_manifest({ExecutableKind::Reducer, "overwrite", "1.0.0", digest('d')}),
                        [](const json&, const json& incoming) { return incoming; });
    builder.add_reducer(lease_manifest({ExecutableKind::Reducer, "append", "1.0.0", digest('e')}),
                        [](const json& current, const json& incoming) {
                            auto merged = current.is_array() ? current : json::array();
                            for (const auto& item : incoming) merged.push_back(item);
                            return merged;
                        });
    return std::move(builder).build();
}

json fan_out_core() {
    const auto edge = [](const char* from, const char* to) { return json{{"from", from}, {"to", to}}; };
    return json{{"schema_version", 1},
                {"name", "main"},
                {"channels", json{{"value", json{{"reducer", "overwrite"}, {"initial", ""}}},
                                  {"items", json{{"reducer", "append"}, {"initial", json::array()}}},
                                  {"started", json{{"reducer", "overwrite"}, {"initial", false}}},
                                  {"done", json{{"reducer", "overwrite"}, {"initial", false}}},
                                  {"finished", json{{"reducer", "overwrite"}, {"initial", false}}}}},
                {"nodes", json{{"head", json{{"type", "lt.head"}}},
                               {"fan", json{{"type", "lt.fan"}}},
                               {"work", json{{"type", "lt.work"}}},
                               {"tail", json{{"type", "lt.tail"}}},
                               {"last", json{{"type", "lt.last"}}}}},
                {"edges", json::array({edge("__start__", "head"), edge("head", "fan"), edge("fan", "__end__"),
                                       edge("work", "tail"), edge("tail", "last"), edge("last", "__end__")})},
                {"conditional_edges", json::array()}};
}

/// What a lease-holding root is granted: no child budgets.
RunBudget lease_budget() { return RunBudget{kWallMs, 1'000, 1'000, 2, 1, 20, 0, 0, 0}; }

/// The stores one process opens: on one SQLite file (a crash image is one consistent copy), or views of stores that
/// several in-memory processes share.
struct ProcessStores {
    std::shared_ptr<ProgramStore>            programs;
    std::shared_ptr<ProgramTransitionStore>  transitions;
    std::shared_ptr<graph::CheckpointStore>  checkpoints;
};

ProcessStores durable_stores(World& world, const std::filesystem::path& directory) {
    const auto file = (directory / "durable.sqlite").string();
    return ProcessStores{
        std::make_shared<SQLiteProgramStore>(file),
        std::make_shared<CountingTransitionStore>(std::make_shared<SQLiteProgramTransitionStore>(file), world.crash),
        std::make_shared<CountingCheckpointStore>(std::make_shared<graph::SqliteCheckpointStore>(file), world.crash)};
}

class LeaseHost {
public:
    explicit LeaseHost(ProcessStores stores) : stores_(std::move(stores)), registry_(lease_registry()) {
        AdmissionProfileBuilder profile;
        profile.id("root-lease-profile").semantic_version("1.0.0").registry(registry_)
            .mode(AdmissionMode::MultiTenant).max_program_schema_version(2)
            .allow_source_kind(SourceKind::CppBuilder).allow_effect_mode(EffectMode::Brokered);
        for (const auto& identity : registry_.identities()) profile.allow_executable(identity);
        profile_.emplace(std::move(profile).build());

        CatalogConfig catalog{stores_.programs, registry_, std::make_shared<EngineGenerationCache>(), "root-lease/v1"};
        catalog.worker_count      = 2;
        catalog.capability_binder = program::detail::single_provider_binder(std::nullopt, "root-lease");
        catalog_                  = std::make_shared<ProgramCatalog>(std::move(catalog));

        RuntimeConfig runtime;
        runtime.catalog           = catalog_;
        runtime.checkpoints       = stores_.checkpoints;
        runtime.transitions       = stores_.transitions;
        runtime.scheduler_threads = 2;
        runtime_                  = std::make_unique<ProgramRuntime>(std::move(runtime));
    }
    ~LeaseHost() {
        runtime_.reset();
        catalog_.reset();
    }

    ProgramVersion admit() {
        json document{
            {"program_schema_version", 2U},
            {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"root", {{"op", "call_core"}, {"name", "main"}, {"definition", fan_out_core()}}},
            {"declared_budget_requirements",
             json::array({json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 100000}},
                          json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
                          json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 10000}},
                          json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 2}},
                          json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 1}},
                          json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 20}},
                          json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
        ProgramCompiler compiler(registry_, {"root-lease/v1"});
        const auto bundle =
            compiler.compile(ProgramSource::from_cpp_builder("root-lease", 2, std::move(document)));
        PolicySnapshotBuilder policy;
        policy.id("root-lease-policy").semantic_version("1.0.0").owner_scope(kOwner).admission_profile(*profile_)
            .budget_ceiling(BudgetLimits{100000, 10000, 10000, 2, 1, 20, 1, 0, 0});
        return catalog_->admit(bundle, ProgramAdmission{kOwner, *profile_, std::move(policy).build(), {}});
    }

    ProgramHandle start(const ProgramVersion& version, const std::string& run_id) {
        ProgramInvocation invocation{json::object(), lease_budget(), "trace-" + run_id, {}};
        invocation.requested_run_id = run_id;
        return runtime_->start(kOwner, version, std::move(invocation));
    }
    ProgramResult run(const ProgramVersion& version, const std::string& run_id) {
        return start(version, run_id).wait();
    }
    ProgramHandle  reconnect(const std::string& run_id) { return runtime_->reconnect(kOwner, run_id); }
    ProgramRuntime& runtime() { return *runtime_; }

private:
    ProcessStores                   stores_;
    RegistrySnapshot                registry_;
    std::optional<AdmissionProfile> profile_;
    std::shared_ptr<ProgramCatalog> catalog_;
    std::unique_ptr<ProgramRuntime> runtime_;
};

/// The runtime reads this clock while the object lives. It starts at the real times and moves only when the test
/// advances it, so a recorded timestamp, and the wall time a recovered attempt is granted, are exact.
struct ScopedSimulatedClock {
    async::detail::SimulatedClock clock{std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
    ScopedSimulatedClock() { async::detail::set_simulated_clock_for_testing(&clock); }
    ~ScopedSimulatedClock() { async::detail::set_simulated_clock_for_testing(nullptr); }
    ScopedSimulatedClock(const ScopedSimulatedClock&)            = delete;
    ScopedSimulatedClock& operator=(const ScopedSimulatedClock&) = delete;
};

std::string failure_of(const ProgramResult& result) {
    std::string text = result.failure() ? result.failure()->code + ": " + result.failure()->message
                                        : std::string("no failure");
    if (const auto interrupt = result.interrupt())
        text += "; interrupt " + interrupt->value.dump() +
                (interrupt->pending_effect ? " pending effect " + interrupt->pending_effect->operation_id() : "");
    return text;
}

std::set<std::string> items_of(const json& value) {
    std::set<std::string> items;
    const std::function<void(const json&)> collect = [&](const json& node) {
        if (node.is_string()) {
            for (const auto& send : kSends)
                if (node.get<std::string>() == send) items.insert(send);
        } else if (node.is_array() || node.is_object()) {
            for (const auto& child : node) collect(child);
        }
    };
    collect(value);
    return items;
}

/// The durable state of a run in a store directory.
struct StoredRun {
    std::optional<ProgramRunRecord>      record;
    std::optional<ProgramExecutionLease> lease;
};

StoredRun stored_run(const std::filesystem::path& directory, const std::string& run_id) {
    auto store = std::make_shared<BlockingProgramTransitionStore>(
        std::make_shared<SQLiteProgramTransitionStore>((directory / "durable.sqlite").string()));
    return StoredRun{store->load(kOwner, run_id), store->load_execution_lease(kOwner, run_id)};
}

/// What a store directory holds of the Core run on `thread`: the work values and the plain steps whose writes are in
/// the latest checkpoint or in a pending write under it. Recovery does not run those again.
struct CommittedWork {
    bool                  any_checkpoint = false;
    std::set<std::string> work;
    std::set<std::string> steps;   // "tail", "last"
    std::string           latest;  // the latest checkpoint, for a failure message
};

bool channel_is_set(const json& channels, const char* name) {
    if (!channels.contains(name)) return false;
    const auto& channel = channels[name];
    return channel.is_object() ? channel.value("value", false) : channel == json(true);
}

CommittedWork committed_work(const std::filesystem::path& directory, const std::string& thread) {
    CommittedWork committed;
    if (thread.empty()) return committed;
    graph::SqliteCheckpointStore checkpoints((directory / "durable.sqlite").string());
    const auto latest = checkpoints.load_latest(thread);
    if (!latest) return committed;
    committed.any_checkpoint = true;
    committed.latest = "step " + std::to_string(latest->step) + " " + std::string(to_string(latest->interrupt_phase)) +
                       " next " + json(latest->next_nodes).dump() + " channels " + latest->channel_values.dump();
    if (latest->channel_values.contains("channels")) {
        const auto& channels = latest->channel_values["channels"];
        if (channels.contains("items")) committed.work = items_of(channels["items"]);
        if (channel_is_set(channels, "done")) committed.steps.insert("tail");
        if (channel_is_set(channels, "finished")) committed.steps.insert("last");
    }
    for (const auto& write : checkpoints.get_writes(thread, latest->id)) {
        if (write.node_name == "work")
            for (const auto& item : items_of(write.writes)) committed.work.insert(item);
        if (write.node_name == "tail" || write.node_name == "last") committed.steps.insert(write.node_name);
    }
    return committed;
}

void drop_lease_rows(const std::filesystem::path& directory) {
    Connection db((directory / "durable.sqlite").string(), SQLITE_OPEN_READWRITE);
    ASSERT_EQ(sqlite3_exec(db.db, "DELETE FROM program_execution_leases_v1", nullptr, nullptr, nullptr), SQLITE_OK);
}

// ---------------------------------------------------------------------------
// A crash at every durable point
// ---------------------------------------------------------------------------

struct Probe {
    int                      operations = 0;
    std::set<std::string>    items;
    std::vector<std::string> names;  // the durable operations, in order
};

/// An uncrashed run: how many durable operations it makes and what it ends with.
Probe probe_run() {
    LeaseWorld world(next_world_id());
    TempDir    live("root-lease-probe");
    world.crash.reset(live.path, live.path / "unused", -1, false);
    LeaseHost  host(durable_stores(world, live.path));
    const auto result = host.run(host.admit(), world.run_id());
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    Probe probe{world.crash.ops, items_of(result.output()), world.crash.seen};
    EXPECT_EQ(probe.items, std::set<std::string>(kSends.begin(), kSends.end()));
    // The fan-out step records a pending write per finished send before its checkpoint, so that a crash between
    // them leaves some sends finished and others not.
    std::string operations;
    for (const auto& name : probe.names) operations += name + ", ";
    EXPECT_GE(static_cast<std::size_t>(std::count(probe.names.begin(), probe.names.end(), std::string("put_writes"))),
              kSends.size())
        << operations;
    return probe;
}

/// Runs the Program to its end in a process whose durable state is copied at operation `at`, and returns the
/// thread of its Core run (empty when no checkpoint was saved before the copy).
std::string crash_at(LeaseWorld& world, const std::filesystem::path& live, const std::filesystem::path& image, int at,
                     bool after = false) {
    world.crash.reset(live, image, at, after);
    {
        LeaseHost  host(durable_stores(world, live));
        const auto result = host.run(host.admit(), world.run_id());
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    }
    EXPECT_TRUE(world.crash.taken);
    std::string thread;
    for (const auto& saved : world.crash.threads)
        if (!saved.empty()) thread = saved;
    return thread;
}

TEST(ProgramRootLeaseTakeover, ASendFanOutCrashedAtEveryDurablePointIsTakenOverAndFinishesOnce) {
    ScopedSimulatedClock clock;
    const auto           probe = probe_run();
    ASSERT_GT(probe.operations, 4);
    constexpr std::uint64_t kRestartAfterMs = 1'000;
    // A crash before every durable operation, and one after the last (the terminal publication).
    for (int point = 0; point <= probe.operations; ++point) {
        const bool  crash_after = point == probe.operations;
        const int   at          = crash_after ? probe.operations - 1 : point;
        std::string operations;
        for (std::size_t index = 0; index < probe.names.size(); ++index)
            operations += (index ? ", " : "") + std::to_string(index) + " " + probe.names[index];
        SCOPED_TRACE(std::string("crash ") + (crash_after ? "after" : "before") + " durable operation " +
                     std::to_string(at) + " of: " + operations);
        LeaseWorld world(next_world_id());
        TempDir    live("root-lease-live"), image("root-lease-image");
        const auto thread  = crash_at(world, live.path, image.path, at, crash_after);
        const auto crashed = take_runs(world);
        (void)crashed;
        world.crash.alive = true;
        const auto stored    = stored_run(image.path, world.run_id());
        const auto committed = committed_work(image.path, thread);

        clock.clock.advance(std::chrono::milliseconds(kRestartAfterMs));
        world.crash.reset(image.path, image.path / "unused", -1, false);
        LeaseHost restarted(durable_stores(world, image.path));
        if (!stored.record) {
            try {
                (void)restarted.reconnect(world.run_id());
                ADD_FAILURE() << "a run that was never published was reconnected";
            } catch (const ProgramDiagnosticError& error) {
                EXPECT_EQ(error.diagnostic().code, "P_RUN_NOT_FOUND");
            }
            continue;
        }
        if (stored.record->terminal_result()) {
            const auto result = restarted.reconnect(world.run_id()).wait();
            EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
            EXPECT_EQ(stored_run(image.path, world.run_id()).record->continuation().fence, 1U)
                << "a finished run is not taken over";
            EXPECT_TRUE(take_runs(world).empty());
            continue;
        }
        ASSERT_EQ(stored.record->continuation().state, ContinuationState::Running);
        ASSERT_EQ(stored.record->continuation().fence, 1U);
        ASSERT_TRUE(stored.lease) << "a running root holds its lease";

        const auto result    = restarted.reconnect(world.run_id()).wait();
        const auto recovered = take_runs(world);
        const auto after     = stored_run(image.path, world.run_id());
        ASSERT_TRUE(after.record);
        EXPECT_EQ(after.record->continuation().fence, 2U) << "the run was taken over";
        EXPECT_FALSE(after.lease) << "the takeover deleted the lease, and the recovered attempt holds none";
        if (!committed.any_checkpoint) {
            // A Core call that saved no checkpoint may have started: the host reconciles it.
            EXPECT_EQ(result.status(), ProgramTerminalStatus::Interrupted) << failure_of(result);
            ASSERT_TRUE(result.interrupt());
            EXPECT_TRUE(result.interrupt()->pending_effect);
            EXPECT_TRUE(recovered.empty());
            continue;
        }
        ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
        EXPECT_EQ(items_of(result.output()), probe.items);
        for (const auto& value : kSends) {
            EXPECT_EQ(runs_of(recovered, "work:" + value), committed.work.count(value) ? 0U : 1U)
                << "work " << value << ": an invocation the image holds is not run again; another runs once";
        }
        for (const auto* step : {"tail", "last"})
            EXPECT_EQ(runs_of(recovered, step), committed.steps.count(step) ? 0U : 1U)
                << step << "; latest " << committed.latest;
        EXPECT_LE(runs_of(recovered, "fan"), 1U);
        EXPECT_LE(runs_of(recovered, "head"), 1U);
        // The recovered attempt was granted the wall time left before the absolute deadline the run started with.
        EXPECT_EQ(result.remaining_budget().wall_time_ms, kWallMs - kRestartAfterMs)
            << "recovery grants no wall time";
    }
}

TEST(ProgramRootLeaseTakeover, ARootRecoveredPastItsDeadlineEndsTimedOutWithoutRunning) {
    ScopedSimulatedClock clock;
    const auto           probe = probe_run();
    // The first durable operations publish the start and save the first checkpoint: crash after both.
    LeaseWorld world(next_world_id());
    TempDir    live("root-lease-live"), image("root-lease-image");
    const auto thread = crash_at(world, live.path, image.path, probe.operations - 2);
    (void)thread;
    (void)take_runs(world);
    world.crash.alive = true;
    const auto stored = stored_run(image.path, world.run_id());
    ASSERT_TRUE(stored.record);
    ASSERT_FALSE(stored.record->terminal_result());
    ASSERT_TRUE(stored.lease);

    clock.clock.advance(std::chrono::milliseconds(kWallMs));
    world.crash.reset(image.path, image.path / "unused", -1, false);
    LeaseHost  restarted(durable_stores(world, image.path));
    const auto result = restarted.reconnect(world.run_id()).wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << failure_of(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_RUN_LEASE_EXPIRED");
    EXPECT_TRUE(take_runs(world).empty()) << "a run past its deadline does not run again";
    const auto after = stored_run(image.path, world.run_id());
    EXPECT_EQ(after.record->continuation().fence, 1U) << "the expired run ends at its fence, not taken over";
    EXPECT_FALSE(after.lease);
}

TEST(ProgramRootLeaseTakeover, ARunTakenOverAndCrashedAgainIsTakenOverWithoutALease) {
    ScopedSimulatedClock clock;
    const auto           probe = probe_run();
    LeaseWorld           world(next_world_id());
    TempDir              live("root-lease-live"), first("root-lease-first"), second("root-lease-second");
    // Crash after the first checkpoint of the fan step, before the last operations.
    const auto thread = crash_at(world, live.path, first.path, probe.operations - 2);
    (void)thread;
    (void)take_runs(world);
    world.crash.alive = true;
    ASSERT_TRUE(stored_run(first.path, world.run_id()).lease);

    // The second process crashes right after its takeover committed.
    world.crash.reset(first.path, second.path, -1, true);
    world.crash.armed_when = [](const std::vector<std::string>& seen) { return seen.back() == "publish takeover"; };
    {
        LeaseHost  recovering(durable_stores(world, first.path));
        const auto result = recovering.reconnect(world.run_id()).wait();
        EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    }
    ASSERT_TRUE(world.crash.taken);
    (void)take_runs(world);
    world.crash.alive = true;
    const auto taken = stored_run(second.path, world.run_id());
    ASSERT_TRUE(taken.record);
    EXPECT_EQ(taken.record->continuation().fence, 2U);
    EXPECT_FALSE(taken.lease);

    clock.clock.advance(std::chrono::milliseconds(2'000));
    world.crash.reset(second.path, second.path / "unused", -1, false);
    LeaseHost  third(durable_stores(world, second.path));
    const auto result = third.reconnect(world.run_id()).wait();
    ASSERT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    EXPECT_EQ(items_of(result.output()), probe.items);
    EXPECT_EQ(result.remaining_budget().wall_time_ms, kWallMs - 2'000) << "recovery grants no wall time";
    const auto after = stored_run(second.path, world.run_id());
    EXPECT_EQ(after.record->continuation().fence, 3U);
    EXPECT_FALSE(after.lease);
}

TEST(ProgramRootLeaseTakeover, ARunTakenOverBeforeAndRecoveredPastItsDeadlineEndsTimedOut) {
    ScopedSimulatedClock clock;
    const auto           probe = probe_run();
    LeaseWorld           world(next_world_id());
    TempDir              live("root-lease-live"), first("root-lease-first"), second("root-lease-second");
    (void)crash_at(world, live.path, first.path, probe.operations - 2);
    (void)take_runs(world);
    world.crash.alive = true;
    // The second process crashes right after its takeover committed: fence 2, no lease.
    world.crash.reset(first.path, second.path, -1, true);
    world.crash.armed_when = [](const std::vector<std::string>& seen) { return seen.back() == "publish takeover"; };
    {
        LeaseHost recovering(durable_stores(world, first.path));
        (void)recovering.reconnect(world.run_id()).wait();
    }
    ASSERT_TRUE(world.crash.taken);
    (void)take_runs(world);
    world.crash.alive = true;
    ASSERT_EQ(stored_run(second.path, world.run_id()).record->continuation().fence, 2U);
    ASSERT_FALSE(stored_run(second.path, world.run_id()).lease);

    // Past the absolute deadline the run started with, the run ends TimedOut at fence 2 without running again.
    clock.clock.advance(std::chrono::milliseconds(kWallMs));
    world.crash.reset(second.path, second.path / "unused", -1, false);
    LeaseHost  third(durable_stores(world, second.path));
    const auto result = third.reconnect(world.run_id()).wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::TimedOut) << failure_of(result);
    ASSERT_TRUE(result.failure());
    EXPECT_EQ(result.failure()->code, "P_RUN_LEASE_EXPIRED");
    EXPECT_TRUE(result.failure()->witness.contains("deadline_ms")) << result.failure()->witness.dump();
    EXPECT_TRUE(take_runs(world).empty()) << "a run past its deadline does not run again";
    const auto after = stored_run(second.path, world.run_id());
    ASSERT_TRUE(after.record && after.record->terminal_result());
    EXPECT_EQ(after.record->continuation().fence, 2U);
    EXPECT_FALSE(after.lease);
}

TEST(ProgramRootLeaseTakeover, ARunningRootThatLostItsLeaseBeforeAnyTakeoverIsCorrupt) {
    const auto probe = probe_run();
    LeaseWorld world(next_world_id());
    TempDir    live("root-lease-live"), image("root-lease-image");
    (void)crash_at(world, live.path, image.path, probe.operations - 2);
    (void)take_runs(world);
    world.crash.alive = true;
    ASSERT_TRUE(stored_run(image.path, world.run_id()).lease);
    drop_lease_rows(image.path);

    world.crash.reset(image.path, image.path / "unused", -1, false);
    LeaseHost restarted(durable_stores(world, image.path));
    try {
        (void)restarted.reconnect(world.run_id());
        ADD_FAILURE() << "a root at fence 1 without its lease was recovered";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_RUN_LEASE_MISSING");
    }
    EXPECT_TRUE(take_runs(world).empty());
    EXPECT_EQ(stored_run(image.path, world.run_id()).record->continuation().fence, 1U);
}

// ---------------------------------------------------------------------------
// Two live processes over shared stores
// ---------------------------------------------------------------------------

struct SharedBackend {
    std::shared_ptr<InMemoryProgramStore>           programs = std::make_shared<InMemoryProgramStore>();
    std::shared_ptr<InMemoryProgramTransitionStore> transitions =
        std::make_shared<InMemoryProgramTransitionStore>();
    std::shared_ptr<graph::InMemoryCheckpointStore> checkpoints = std::make_shared<graph::InMemoryCheckpointStore>();
};

struct Process {
    explicit Process(SharedBackend& backend)
        : view(std::make_shared<ProcessView>(backend.transitions)),
          checkpoints(std::make_shared<SaveCountingCheckpointStore>(backend.checkpoints)),
          host(ProcessStores{backend.programs, view, checkpoints}) {}
    // A test that stops early leaves no save waiting.
    ~Process() { checkpoints->save_gate.release(); }
    std::shared_ptr<ProcessView>                 view;
    std::shared_ptr<SaveCountingCheckpointStore> checkpoints;
    LeaseHost                                    host;
};

std::uint64_t stored_fence(SharedBackend& backend, const std::string& run_id) {
    const auto record = backend.transitions->load_sync(kOwner, run_id);
    return record ? record->continuation().fence : 0;
}

/// The checkpoints of the Core thread `process` saved on, in the shared store, oldest first, for a failure message.
std::string thread_checkpoints(SharedBackend& backend, const std::string& thread) {
    std::string text = "thread " + thread + ":";
    auto        all  = backend.checkpoints->list(thread, 1000);
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        text += " [" + it->id.substr(0, 8) + " parent " + it->parent_id.substr(0, 8) + " step " +
                std::to_string(it->step) + " " + std::string(to_string(it->interrupt_phase)) + "]";
    return text;
}

bool has_notice(const ProgramHandle& handle, std::string_view code) {
    const auto notices = handle.notices();
    return std::any_of(notices.begin(), notices.end(), [&](const auto& notice) { return notice.code == code; });
}

TEST(ProgramRootLeaseTakeover, AProcessThatWasTakenOverSavesNoFurtherCheckpointAndStops) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();

    auto source = original.host.start(version, world.run_id());
    world.tail_gate.wait_entered_or([] { return false; });
    ASSERT_TRUE(backend.transitions->load_execution_lease_sync(kOwner, world.run_id()));

    // Another process recovers the run while the original one is inside the tail's step.
    const auto before    = thread_checkpoints(backend, original.checkpoints->saved_thread());
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered) << "; " << before;
    EXPECT_EQ(items_of(recovered.output()), std::set<std::string>(kSends.begin(), kSends.end()));
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_FALSE(backend.transitions->load_execution_lease_sync(kOwner, world.run_id()));
    const auto saves_before_release = original.checkpoints->saves.load();

    // The original process finishes the step it is in: it saves nothing more and does not reach the next step.
    world.tail_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_TRUE(has_notice(source, "P_RUN_TAKEN_OVER"));
    EXPECT_EQ(original.checkpoints->saves.load(), saves_before_release)
        << "the original process saved a checkpoint after the takeover";
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "tail"), 2U) << "the step in flight in the original process, and the recovered one";
    EXPECT_EQ(runs_of(runs, "last"), 1U) << "only the recovering process reaches the next step";
    for (const auto& value : kSends) EXPECT_EQ(runs_of(runs, "work:" + value), 1U) << value;
    const auto stored = backend.transitions->load_sync(kOwner, world.run_id());
    ASSERT_TRUE(stored && stored->terminal_result());
    EXPECT_EQ(stored->terminal_result()->status(), ProgramTerminalStatus::Completed);
}

TEST(ProgramRootLeaseTakeover, ASaveThatATakeoverOvertookStopsTheProcessBeforeItsNextStep) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    // The original process holds the save that ends the tail's step, after its fence compare passed.
    original.checkpoints->hold_when = [](const graph::Checkpoint& checkpoint) {
        return checkpoint.channel_values.contains("channels") &&
               channel_is_set(checkpoint.channel_values["channels"], "done");
    };
    original.checkpoints->save_gate.arm();

    auto source = original.host.start(version, world.run_id());
    original.checkpoints->save_gate.wait_entered_or([] { return false; });
    const auto saves_before = original.checkpoints->saves.load();

    // Another process takes the run over and finishes it; the tail's pending write is replayed, not run again.
    const auto before    = thread_checkpoints(backend, original.checkpoints->saved_thread());
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered) << "; " << before;
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);

    // The held save lands after the takeover; the original process then stops before the next step.
    original.checkpoints->save_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(original.checkpoints->saves.load(), saves_before + 1) << "the overtaken save lands, and only it";
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "tail"), 1U);
    EXPECT_EQ(runs_of(runs, "last"), 1U) << "the original process ran the step after its overtaken save";
    for (const auto& value : kSends) EXPECT_EQ(runs_of(runs, "work:" + value), 1U) << value;
}

TEST(ProgramRootLeaseTakeover, AnAttemptThatEndsAfterATakeoverWithoutACheckpointSavesNone) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.fail_head = true;
    world.head_gate.arm();

    auto source = original.host.start(version, world.run_id());
    world.head_gate.wait_entered_or([] { return false; });
    // Nothing is saved yet: the recovering process stops for the host to reconcile the Core call.
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    EXPECT_EQ(recovered.status(), ProgramTerminalStatus::Interrupted) << failure_of(recovered);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    const auto saves_before = original.checkpoints->saves.load();

    // The original attempt fails without having saved anything since the takeover: it saves no terminal checkpoint.
    world.head_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(original.checkpoints->saves.load(), saves_before) << "the original attempt saved a checkpoint";
}

TEST(ProgramRootLeaseTakeover, AFailedRunReadBeforeACheckpointIsNotATakeover) {
    SharedBackend backend;
    Process       only(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = only.host.admit();
    // From the fan step on, every read of the run record fails, including those made before a checkpoint save.
    world.on_fan = [view = only.view] { view->fail_run_reads.store(true); };
    auto       handle = only.host.start(version, world.run_id());
    const auto result = handle.wait();
    only.view->fail_run_reads.store(false);
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    EXPECT_FALSE(has_notice(handle, "P_RUN_TAKEN_OVER"));
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "tail"), 1U);
    EXPECT_EQ(runs_of(runs, "last"), 1U);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
}

/// Starts a migration of `source` on another thread and lets the source past its fan step once the migration has
/// read the source's lineage, the last read before it asks for a safe point. The source is held in its fan step and
/// reads no lineage there, so the first read after the hook is installed is the migration's.
struct MigrationRace {
    MigrationRace(Process& process, LeaseWorld& world, const ProgramVersion& version, ProgramHandle source)
        : process_(process), world_(world) {
        process_.view->on_run_lineage_read = [this] { world_.fan_gate.release(); };
        migration = std::async(std::launch::async, [this, &version, source]() -> std::optional<ProgramResult> {
            try {
                return process_.host.runtime()
                    .migrate_graph(source, ProgramGraphMigrationTarget{version.id(), world_.run_id("target"), {}})
                    .wait();
            } catch (const std::exception&) {
                return std::nullopt;
            }
        });
    }
    ~MigrationRace() { process_.view->on_run_lineage_read = nullptr; }
    std::future<std::optional<ProgramResult>> migration;

private:
    Process&    process_;
    LeaseWorld& world_;
};

TEST(ProgramRootLeaseTakeover, ATakeoverDuringASafePointPublicationRefusesTheMigration) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    Gate          publication;
    publication.arm();
    original.view->before_safe_point = [&] { publication.pass(); };
    world.fan_gate.arm();

    auto                    source = original.host.start(version, world.run_id());
    std::atomic<bool>       source_done{false};
    auto                    source_end = std::async(std::launch::async, [&] {
        const auto result = source.wait();
        source_done.store(true);
        publication.notify();
        return result;
    });
    std::optional<MigrationRace> race;
    race.emplace(original, world, version, source);
    const ReleaseOnExit release_all{[&] {
        publication.release();
        world.fan_gate.release();
    }};
    ASSERT_TRUE(publication.wait_entered_or([&] { return source_done.load(); }))
        << "the source ended before the migration reached a safe point";

    // The migration's safe point is being published; another process takes the run over first.
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_FALSE(backend.transitions->load_execution_lease_sync(kOwner, world.run_id()));

    publication.release();
    EXPECT_FALSE(race->migration.get()) << "a migration of a source that was taken over published a successor";
    const auto stale = source_end.get();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    race.reset();
    const auto lineage = backend.transitions->load_run_lineage_sync(kOwner, world.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 1U);
    EXPECT_FALSE(backend.transitions->load_graph_migration_capsule_sync(kOwner, world.run_id(), lineage->id()));
    EXPECT_FALSE(backend.transitions->load_sync(kOwner, world.run_id("target")));
    EXPECT_EQ(runs_of(take_runs(world), "tail"), 1U);
}

TEST(ProgramRootLeaseTakeover, ASourceHeldAtAPublishedSafePointIsNotTakenOver) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    Gate          published;
    published.arm();
    original.view->after_safe_point = [&] { published.pass(); };
    world.fan_gate.arm();

    auto                    source = original.host.start(version, world.run_id());
    std::atomic<bool>       source_done{false};
    auto                    source_end = std::async(std::launch::async, [&] {
        const auto result = source.wait();
        source_done.store(true);
        published.notify();
        return result;
    });
    std::optional<MigrationRace> race;
    race.emplace(original, world, version, source);
    const ReleaseOnExit release_all{[&] {
        published.release();
        world.fan_gate.release();
    }};
    ASSERT_TRUE(published.wait_entered_or([&] { return source_done.load(); }))
        << "the source ended before the migration reached a safe point";

    // The safe point is published: the source is held for the migration and another process cannot take it over.
    try {
        (void)recovering.host.reconnect(world.run_id());
        ADD_FAILURE() << "a source held for a migration was taken over";
    } catch (const ProgramDiagnosticError& error) {
        EXPECT_EQ(error.diagnostic().code, "P_GRAPH_MIGRATION_HELD");
    }
    EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);

    published.release();
    const auto target = race->migration.get();
    ASSERT_TRUE(target) << "the migration failed";
    EXPECT_EQ(target->status(), ProgramTerminalStatus::Completed) << failure_of(*target);
    (void)source_end.get();
    race.reset();
    const auto lineage = backend.transitions->load_run_lineage_sync(kOwner, world.run_id());
    ASSERT_TRUE(lineage);
    EXPECT_EQ(lineage->active_generation(), 2U);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
}

// ---------------------------------------------------------------------------
// The store: a takeover and the lease
// ---------------------------------------------------------------------------

TEST(ProgramRootLeaseTakeover, TheStoreTakesOverALeasedRunOnlyWithItsLeaseAndDeletesIt) {
    SharedBackend backend;
    Process       process(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = process.host.admit();
    world.fan_gate.arm();
    auto source = process.host.start(version, world.run_id());
    world.fan_gate.wait_entered_or([] { return false; });

    auto&      store  = *backend.transitions;
    const auto record = store.load_sync(kOwner, world.run_id());
    const auto lease  = store.load_execution_lease_sync(kOwner, world.run_id());
    ASSERT_TRUE(record && lease);
    const auto head = record->journal_head();

    // A takeover given no lease refuses a run that holds one; one given another lease refuses too.
    EXPECT_EQ(store.compare_publish_takeover_sync(kOwner, world.run_id(), head, 1),
              ProgramTransitionPublishResult::Conflict);
    auto other_data      = ProgramExecutionLeaseData{lease->owner_scope(), lease->run_id(), lease->attempt(),
                                                lease->program_version_id(), lease->bundle_id(), lease->core_name(),
                                                lease->core_generation_id(), lease->core_thread_id(),
                                                "another-holder", lease->acquired_at_ms(), lease->expires_at_ms()};
    const ProgramExecutionLease other(std::move(other_data));
    EXPECT_EQ(store.compare_publish_takeover_execution_sync(kOwner, world.run_id(), head, 1, other),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.load_sync(kOwner, world.run_id())->continuation().fence, 1U);
    ASSERT_TRUE(store.load_execution_lease_sync(kOwner, world.run_id()));

    // With its lease: the fence moves and the lease is gone, in one step.
    EXPECT_EQ(store.compare_publish_takeover_execution_sync(kOwner, world.run_id(), head, 1, *lease),
              ProgramTransitionPublishResult::Published);
    const auto taken = store.load_sync(kOwner, world.run_id());
    EXPECT_EQ(taken->continuation().fence, 2U);
    EXPECT_FALSE(store.load_execution_lease_sync(kOwner, world.run_id()));
    // The same lease cannot take the run over again, and a run without a lease is not taken over with one.
    EXPECT_EQ(store.compare_publish_takeover_execution_sync(kOwner, world.run_id(), taken->journal_head(), 2, *lease),
              ProgramTransitionPublishResult::Conflict);
    EXPECT_EQ(store.load_sync(kOwner, world.run_id())->continuation().fence, 2U);

    world.fan_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
}

}  // namespace
