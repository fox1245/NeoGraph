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

#include <neograph/graph/engine.h>

#include "../src/core/runtime_clock.h"
#include "../src/program/run_control.h"
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
    template <class Done>
    void pass_or(Done done) {
        struct ActiveRegistration {
            Gate* self;
            explicit ActiveRegistration(Gate* g) : self(g) { register_active_gate(g); }
            ~ActiveRegistration() { unregister_active_gate(self); }
        } reg(this);
        std::unique_lock lock(mutex_);
        if (!armed_) return;
        armed_   = false;
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_ || done(); });
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
        struct ActiveRegistration {
            Gate* self;
            explicit ActiveRegistration(Gate* g) : self(g) { register_active_gate(g); }
            ~ActiveRegistration() { unregister_active_gate(self); }
        } reg(this);
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
    static std::mutex& active_gates_mutex() {
        static std::mutex m;
        return m;
    }
    static std::set<Gate*>& active_gates() {
        static std::set<Gate*> s;
        return s;
    }
    static void on_any_attempt_finished(std::string_view, std::uint64_t) noexcept {
        std::vector<Gate*> snapshot;
        {
            std::lock_guard lock(active_gates_mutex());
            snapshot.assign(active_gates().begin(), active_gates().end());
        }
        for (auto* gate : snapshot) {
            if (gate) gate->notify();
        }
    }
    static void register_active_gate(Gate* gate) {
        std::lock_guard lock(active_gates_mutex());
        if (active_gates().empty()) {
            program::detail::set_attempt_finished_observer_for_testing(&Gate::on_any_attempt_finished);
        }
        active_gates().insert(gate);
    }
    static void unregister_active_gate(Gate* gate) {
        std::lock_guard lock(active_gates_mutex());
        active_gates().erase(gate);
        if (active_gates().empty()) {
            program::detail::set_attempt_finished_observer_for_testing(nullptr);
        }
    }

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
    /// The tail step throws NodeInterrupt until resumed with a resume_value.
    std::atomic<bool> interrupt_tail{false};
    /// The last step throws NodeInterrupt until resumed with a resume_value.
    std::atomic<bool> interrupt_last{false};
    std::string gated_work;  // the work value work_gate holds
    /// Runs in the fan node's body, after it is recorded.
    std::function<void()> on_fan;
    /// Runs in the tail node's body, after it is recorded.
    std::function<asio::awaitable<void>(const std::string&)> on_tail_async;
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
        if (label_ == "tail" && world.on_tail_async) co_await world.on_tail_async(in.ctx.thread_id);
        if (gate_) (world.*gate_).pass();
        if (fail_ && (world.*fail_).load()) throw std::runtime_error("step " + label_ + " failed");
        if (label_ == "tail" && world.interrupt_tail.load() && !in.ctx.resume_value) {
            throw graph::NodeInterrupt("approve tail", json{{"ask", "tail"}});
        }
        if (label_ == "last" && world.interrupt_last.load() && !in.ctx.resume_value) {
            throw graph::NodeInterrupt("approve last", json{{"ask", "last"}});
        }
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
    /// Runs before compare_publish / compare_publish_execution reaches the shared store.
    std::function<void(const ProgramTransitionPublication&)> before_compare_publish;
    /// Runs before compare_publish_takeover / compare_publish_takeover_execution reaches the shared store.
    std::function<void()> before_takeover;
    /// Runs when a lineage is read by run id.
    std::function<void()> on_run_lineage_read;
    /// Runs when effective runtime state is read by run id.
    std::function<void()> on_effective_runtime_state_read;
    /// While set, reading a run record fails.
    std::atomic<bool> fail_run_reads{false};
    /// While set, takeover CAS returns Conflict without mutating the shared store.
    std::atomic<bool> force_takeover_conflict{false};
    std::atomic<int>         published_transitions{0};
    std::atomic<int>         takeover_attempts{0};
    std::atomic<int>         published_takeovers{0};
    mutable std::atomic<int> publication_head_loads{0};

    std::optional<ProgramRunRecord> load_sync(std::string_view owner, std::string_view run) const override {
        if (fail_run_reads.load()) throw std::runtime_error("injected run record read failure");
        return inner_->load(owner, run);
    }
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner, std::string_view run) const override {
        return inner_->latest(owner, run);
    }
    std::optional<ProgramRunPublicationHead> load_run_publication_head_sync(std::string_view owner,
                                                                            std::string_view run) const override {
        publication_head_loads.fetch_add(1);
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
        auto state = inner_->load_effective_runtime_state(owner, run);
        if (on_effective_runtime_state_read) on_effective_runtime_state_read();
        return state;
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
        if (before_compare_publish) before_compare_publish(publication);
        const auto res = inner_->compare_publish(owner, head, expected_fence, std::move(publication));
        if (res == ProgramTransitionPublishResult::Published) ++published_transitions;
        return res;
    }
    ProgramTransitionPublishResult compare_publish_execution_sync(std::string_view owner, std::string_view head,
                                                                  std::uint64_t expected_fence,
                                                                  ProgramTransitionPublication publication,
                                                                  std::optional<ProgramExecutionLease> expected,
                                                                  std::optional<ProgramExecutionLease> next) override {
        if (before_compare_publish) before_compare_publish(publication);
        const auto res = inner_->compare_publish_execution(owner, head, expected_fence, std::move(publication),
                                                           std::move(expected), std::move(next));
        if (res == ProgramTransitionPublishResult::Published) ++published_transitions;
        return res;
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
        ++takeover_attempts;
        if (before_takeover) before_takeover();
        if (force_takeover_conflict.load()) return ProgramTransitionPublishResult::Conflict;
        const auto res = inner_->compare_publish_takeover(owner, run_id, head, expected_fence);
        if (res == ProgramTransitionPublishResult::Published) ++published_takeovers;
        return res;
    }
    ProgramTransitionPublishResult compare_publish_takeover_execution_sync(
        std::string_view owner, std::string_view run_id, std::string_view head, std::uint64_t expected_fence,
        ProgramExecutionLease expected_lease) override {
        ++takeover_attempts;
        if (before_takeover) before_takeover();
        if (force_takeover_conflict.load()) return ProgramTransitionPublishResult::Conflict;
        const auto res = inner_->compare_publish_takeover_execution(owner, run_id, head, expected_fence,
                                                                    std::move(expected_lease));
        if (res == ProgramTransitionPublishResult::Published) ++published_takeovers;
        return res;
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
    std::atomic<int>                              put_writes_calls{0};
    std::atomic<int>                              clear_writes_calls{0};
    std::atomic<int>                              delete_thread_calls{0};
    std::atomic<std::uint64_t>                    inject_refusal_stored_fence{0};
    std::function<bool(const graph::Checkpoint&)> hold_when;
    Gate                                          save_gate;
    std::function<void(const graph::PendingWrite&)> before_put_writes;
    std::function<void(std::uint64_t)>            before_raise_fence;
    std::function<void(std::uint64_t)>            after_raise_fence;
    std::function<void()>                         on_load_checkpoint;

    std::string saved_thread() const {
        std::lock_guard lock(mutex_);
        return saved_thread_;
    }
    std::uint64_t last_saved_fence() const {
        std::lock_guard lock(mutex_);
        return last_saved_fence_;
    }
    std::vector<std::uint64_t> raised_fences() const {
        std::lock_guard lock(mutex_);
        return raised_fences_;
    }

    bool supports_write_fence() const override { return inner_->supports_write_fence(); }

    void save(const graph::Checkpoint& checkpoint) override {
        save(checkpoint, {});
    }
    void save(const graph::Checkpoint& checkpoint, const graph::CheckpointWriteFence& fence) override {
        hold(checkpoint);
        if (const auto injected = inject_refusal_stored_fence.exchange(0); injected > 0 && fence.active()) {
            throw graph::CheckpointWriteFenceError(
                graph::CheckpointWriteFenceRefusal{fence.owner_scope, fence.run_id, fence.fence, injected});
        }
        inner_->save(checkpoint, fence);
        saved(checkpoint, fence);
    }
    void publish_managed_budget_checkpoint(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                           const graph::Checkpoint& checkpoint) override {
        publish_managed_budget_checkpoint(lease, checkpoint, {});
    }
    void publish_managed_budget_checkpoint(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                           const graph::Checkpoint& checkpoint,
                                           const graph::CheckpointWriteFence& fence) override {
        hold(checkpoint);
        inner_->publish_managed_budget_checkpoint(lease, checkpoint, fence);
        saved(checkpoint, fence);
    }
    void put_writes(const std::string& thread, const std::string& parent, const graph::PendingWrite& write) override {
        put_writes(thread, parent, write, {});
    }
    void put_writes(const std::string& thread, const std::string& parent, const graph::PendingWrite& write,
                    const graph::CheckpointWriteFence& fence) override {
        if (before_put_writes) before_put_writes(write);
        if (const auto injected = inject_refusal_stored_fence.exchange(0); injected > 0 && fence.active()) {
            throw graph::CheckpointWriteFenceError(
                graph::CheckpointWriteFenceRefusal{fence.owner_scope, fence.run_id, fence.fence, injected});
        }
        inner_->put_writes(thread, parent, write, fence);
        ++put_writes_calls;
    }
    std::optional<graph::Checkpoint> load_latest(const std::string& thread) override {
        if (on_load_checkpoint) on_load_checkpoint();
        return inner_->load_latest(thread);
    }
    std::optional<graph::Checkpoint> load_by_id(const std::string& id) override {
        if (on_load_checkpoint) on_load_checkpoint();
        return inner_->load_by_id(id);
    }
    std::vector<graph::Checkpoint> list(const std::string& thread, int limit) override {
        return inner_->list(thread, limit);
    }
    void delete_thread(const std::string& thread) override {
        delete_thread(thread, {});
    }
    void delete_thread(const std::string& thread, const graph::CheckpointWriteFence& fence) override {
        if (!fence.active() || fence.fence <= 1) ++delete_thread_calls;
        inner_->delete_thread(thread, fence);
    }
    std::vector<graph::PendingWrite> get_writes(const std::string& thread, const std::string& parent) override {
        return inner_->get_writes(thread, parent);
    }
    void clear_writes(const std::string& thread, const std::string& parent) override {
        clear_writes(thread, parent, {});
    }
    void clear_writes(const std::string& thread, const std::string& parent,
                      const graph::CheckpointWriteFence& fence) override {
        if (!fence.active() || fence.fence <= 1) ++clear_writes_calls;
        inner_->clear_writes(thread, parent, fence);
    }
    std::uint64_t raise_fence(const std::string& owner_scope, const std::string& run_id,
                              std::uint64_t min_fence) override {
        if (before_raise_fence) before_raise_fence(min_fence);
        {
            std::lock_guard lock(mutex_);
            raised_fences_.push_back(min_fence);
        }
        const auto result = inner_->raise_fence(owner_scope, run_id, min_fence);
        if (after_raise_fence) after_raise_fence(result);
        return result;
    }
    std::uint64_t load_fence(const std::string& owner_scope, const std::string& run_id) override {
        return inner_->load_fence(owner_scope, run_id);
    }
    bool requires_managed_budget(const std::string& thread) override {
        return inner_->requires_managed_budget(thread);
    }
    std::shared_ptr<graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const graph::ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment) override {
        return inner_->acquire_managed_budget_lease(scope, expected_checkpoint_id, expected_checkpoint_commitment);
    }
    std::shared_ptr<graph::OwnedManagedBudgetLease> acquire_managed_budget_lease(
        const graph::ManagedBudgetLeaseScope& scope, const std::string& expected_checkpoint_id,
        const std::string& expected_checkpoint_commitment, const graph::CheckpointWriteFence& fence) override {
        return inner_->acquire_managed_budget_lease(
            scope, expected_checkpoint_id, expected_checkpoint_commitment, fence);
    }
    graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest) override {
        return inner_->begin_managed_budget_effect(lease, effect_id, exact_claim_amount, prepared_request_digest);
    }
    graph::ManagedBudgetEffectReceipt begin_managed_budget_effect(
        const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease, const std::string& effect_id,
        std::uint64_t exact_claim_amount, const std::string& prepared_request_digest,
        const graph::CheckpointWriteFence& fence) override {
        return inner_->begin_managed_budget_effect(
            lease, effect_id, exact_claim_amount, prepared_request_digest, fence);
    }
    void settle_managed_budget_effect(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                      const graph::ManagedBudgetEffectReceipt& effect,
                                      sp::runtime::Result genuine_outcome,
                                      const UsageAccumulator::AuthoritySnapshot& authority) override {
        inner_->settle_managed_budget_effect(lease, effect, std::move(genuine_outcome), authority);
    }
    void settle_managed_budget_effect(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                      const graph::ManagedBudgetEffectReceipt& effect,
                                      sp::runtime::Result genuine_outcome,
                                      const UsageAccumulator::AuthoritySnapshot& authority,
                                      const graph::CheckpointWriteFence& fence) override {
        inner_->settle_managed_budget_effect(lease, effect, std::move(genuine_outcome), authority, fence);
    }
    void release_managed_budget_lease(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease) override {
        inner_->release_managed_budget_lease(lease);
    }
    void release_managed_budget_lease(const std::shared_ptr<graph::OwnedManagedBudgetLease>& lease,
                                      const graph::CheckpointWriteFence& fence) override {
        inner_->release_managed_budget_lease(lease, fence);
    }
    bool retains_native_checkpoint() const noexcept override { return inner_->retains_native_checkpoint(); }
    void publish_managed_budget_fork(const graph::Checkpoint& source, const graph::Checkpoint& forked) override {
        inner_->publish_managed_budget_fork(source, forked);
    }
    void publish_managed_budget_fork(const graph::Checkpoint& source, const graph::Checkpoint& forked,
                                     const graph::CheckpointWriteFence& fence) override {
        inner_->publish_managed_budget_fork(source, forked, fence);
    }

private:
    void hold(const graph::Checkpoint& checkpoint) {
        if (hold_when && hold_when(checkpoint)) save_gate.pass();
    }
    void saved(const graph::Checkpoint& checkpoint, const graph::CheckpointWriteFence& fence) {
        {
            std::lock_guard lock(mutex_);
            saved_thread_     = checkpoint.thread_id;
            last_saved_fence_ = fence.fence;
        }
        ++saves;
    }

    std::shared_ptr<graph::CheckpointStore> inner_;
    mutable std::mutex                      mutex_;
    std::string                             saved_thread_;
    std::uint64_t                           last_saved_fence_{0};
    std::vector<std::uint64_t>              raised_fences_;
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
        runtime.scheduler_threads = 4;
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

    ProgramVersion admit_parallel_three() {
        const auto branch = [] {
            return json{{"op", "call_core"}, {"core", "main"}};
        };
        json document{
            {"program_schema_version", 2U},
            {"input_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"output_contract", {{"schema_version", 1}, {"schema", json::object()}}},
            {"root",
             {{"op", "parallel"},
              {"name", "main"},
              {"definition", fan_out_core()},
              {"branches", json::array({branch(), branch(), branch()})}}},
            {"declared_budget_requirements",
             json::array({json{{"resource", "wall_time_ms"}, {"minimum", 1}, {"maximum", 100000}},
                          json{{"resource", "model_tokens"}, {"minimum", 0}, {"maximum", 10000}},
                          json{{"resource", "monetary_microunits"}, {"minimum", 0}, {"maximum", 10000}},
                          json{{"resource", "max_concurrency"}, {"minimum", 1}, {"maximum", 4}},
                          json{{"resource", "max_program_operations"}, {"minimum", 1}, {"maximum", 4}},
                          json{{"resource", "max_core_steps"}, {"minimum", 1}, {"maximum", 60}},
                          json{{"resource", "max_dynamic_compiles"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_child_depth"}, {"minimum", 0}, {"maximum", 0}},
                          json{{"resource", "max_total_children"}, {"minimum", 0}, {"maximum", 0}}})}};
        ProgramCompiler compiler(registry_, {"root-lease/v1"});
        const auto bundle =
            compiler.compile(ProgramSource::from_cpp_builder("root-lease-par3", 2, std::move(document)));
        PolicySnapshotBuilder policy;
        policy.id("root-lease-par3-policy").semantic_version("1.0.0").owner_scope(kOwner).admission_profile(*profile_)
            .budget_ceiling(BudgetLimits{100000, 10000, 10000, 4, 4, 60, 1, 0, 0});
        return catalog_->admit(bundle, ProgramAdmission{kOwner, *profile_, std::move(policy).build(), {}});
    }

    ProgramHandle start(const ProgramVersion& version, const std::string& run_id) {
        ProgramInvocation invocation{json::object(), lease_budget(), "trace-" + run_id, {}};
        invocation.requested_run_id = run_id;
        return runtime_->start(kOwner, version, std::move(invocation));
    }
    ProgramHandle start_parallel_three(const ProgramVersion& version, const std::string& run_id) {
        ProgramInvocation invocation{
            json::object(), RunBudget{kWallMs, 1'000, 1'000, 3, 4, 60, 0, 0, 0}, "trace-" + run_id, {}};
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
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));
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
    const ReleaseOnExit release_save{[&] { original.checkpoints->save_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(original.checkpoints->save_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));
    const auto saves_before = original.checkpoints->saves.load();

    // Another process takes the run over and finishes it; the tail's pending write is replayed, not run again.
    const auto before    = thread_checkpoints(backend, original.checkpoints->saved_thread());
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered) << "; " << before;
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);

    // The held save is refused by the checkpoint store fence after the takeover; the original process stops before the next step.
    original.checkpoints->save_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(original.checkpoints->saves.load(), saves_before)
        << "the overtaken save is refused by the checkpoint store fence";
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
    const ReleaseOnExit release_head{[&] { world.head_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.head_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));
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
    const ReleaseOnExit release_fan{[&] { world.fan_gate.release(); }};
    auto source = process.host.start(version, world.run_id());
    ASSERT_TRUE(world.fan_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

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

// ---------------------------------------------------------------------------
// Checkpoint-store fencing tokens (T0, T1, T2, T4, T6a, T6b, T7a, T7b, T7c, T8a)
// ---------------------------------------------------------------------------

TEST(ProgramRootLeaseTakeover, T0_StaleSavePausedBeforeWriteTransactionIsRefusedAndDoesNotRedoCompletedStep) {
    SharedBackend backend;
    Process       original(backend), recovering(backend), third(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();

    // Pause Attempt 1 (fence = 1) right before its write transaction for the tail step's checkpoint (holding no store lock).
    original.checkpoints->hold_when = [](const graph::Checkpoint& checkpoint) {
        return checkpoint.channel_values.contains("channels") &&
               channel_is_set(checkpoint.channel_values["channels"], "done") &&
               !channel_is_set(checkpoint.channel_values["channels"], "finished");
    };
    original.checkpoints->save_gate.arm();
    const ReleaseOnExit release_save{[&] { original.checkpoints->save_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(original.checkpoints->save_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));
    const auto saves_before = original.checkpoints->saves.load();

    // Owner B (recovering) takes over at fence = 2, runs tail/last (saving last's checkpoint), and crashes before
    // its terminal transition publication commits (leaving the run record in Running at fence = 2).
    std::atomic<bool> block_recovering_publications{false};
    recovering.view->before_compare_publish = [&](const ProgramTransitionPublication& publication) {
        if (block_recovering_publications.load() || publication.run_record.terminal_result().has_value()) {
            block_recovering_publications.store(true);
            throw std::runtime_error("simulated crash before terminal publication");
        }
    };
    auto recovering_handle = recovering.host.reconnect(world.run_id());
    (void)recovering_handle.wait();
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U);
    const auto latest_after_b = backend.checkpoints->load_latest(original.checkpoints->saved_thread());
    ASSERT_TRUE(latest_after_b);
    ASSERT_TRUE(latest_after_b->channel_values.contains("channels"));
    EXPECT_TRUE(channel_is_set(latest_after_b->channel_values["channels"], "finished"));

    // Release Attempt 1 to begin its write transaction: its stale tail save (wf = 1) is refused by stored_fence = 2.
    original.checkpoints->save_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(original.checkpoints->saves.load(), saves_before) << "stale save must be refused by checkpoint fence";

    // Owner C (third) reconnects: it loads Owner B's last checkpoint (finished = true) and does NOT re-execute last.
    const auto final_result = third.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(final_result.status(), ProgramTerminalStatus::Completed) << failure_of(final_result);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 3U);
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "last"), 1U) << "last must not be re-executed after Attempt 1's stale save is refused";
}

TEST(ProgramRootLeaseTakeover, T1_RaiseFenceCommitsBeforeTakeoverCasClosingMutantM1) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    bool observed_fence_at_cas = false;
    recovering.view->before_takeover = [&] {
        observed_fence_at_cas = true;
        EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U)
            << "raise_fence(2) must commit before compare_publish_takeover";
        graph::Checkpoint stale_cp;
        stale_cp.id             = graph::Checkpoint::generate_id();
        stale_cp.thread_id      = original.checkpoints->saved_thread();
        stale_cp.channel_values = json::object();
        stale_cp.step           = 99;
        stale_cp.timestamp      = 1;
        EXPECT_THROW(
            backend.checkpoints->save(stale_cp, graph::CheckpointWriteFence{kOwner, world.run_id(), 1}),
            graph::CheckpointWriteFenceError);
    };

    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered);
    EXPECT_TRUE(observed_fence_at_cas);
    world.tail_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
}

TEST(ProgramRootLeaseTakeover, T2_ReconnectReadsResumeCheckpointOnlyAfterRaiseAndCasClosingMutantM2) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    world.last_gate.arm();
    const ReleaseOnExit release_gates{[&] {
        world.tail_gate.release();
        world.last_gate.release();
    }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    std::vector<std::string> sequence;
    std::mutex               seq_mutex;
    recovering.checkpoints->before_raise_fence = [&](std::uint64_t) {
        {
            std::lock_guard lock(seq_mutex);
            sequence.push_back("before_raise");
        }
        // Attempt 1 finishes tail and saves tail's checkpoint (wf = 1) after reconnect_core started but before raise_fence(2) commits.
        world.tail_gate.release();
        ASSERT_TRUE(world.last_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));
    };
    recovering.checkpoints->after_raise_fence = [&](std::uint64_t) {
        std::lock_guard lock(seq_mutex);
        sequence.push_back("after_raise");
    };
    recovering.view->before_takeover = [&] {
        std::lock_guard lock(seq_mutex);
        sequence.push_back("takeover_cas");
    };
    recovering.checkpoints->on_load_checkpoint = [&] {
        std::lock_guard lock(seq_mutex);
        sequence.push_back("load_checkpoint");
    };

    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered);
    world.last_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);

    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "tail"), 1U)
        << "recovering must read the resume checkpoint after raise_fence and takeover CAS, observing tail's checkpoint";
    ASSERT_GE(sequence.size(), 4U);
    EXPECT_EQ(sequence[0], "before_raise");
    EXPECT_EQ(sequence[1], "after_raise");
    EXPECT_EQ(sequence[2], "takeover_cas");
    EXPECT_EQ(sequence[3], "load_checkpoint");
}

TEST(ProgramRootLeaseTakeover, T4_SubgraphCheckpointWritesShareRunFenceKeyClosingMutantM4) {
    auto        store  = std::make_shared<graph::InMemoryCheckpointStore>();
    const auto  run_id = std::string("run-subgraph-fence");
    auto        fenced = std::make_shared<graph::FencedCheckpointStore>(store, kOwner, run_id, 1);

    class SubFenceNode final : public graph::GraphNode {
    public:
        SubFenceNode(std::string name, std::shared_ptr<graph::InMemoryCheckpointStore> store, std::string run_id)
            : name_(std::move(name)), store_(std::move(store)), run_id_(std::move(run_id)) {}
        std::string get_name() const override { return name_; }
        asio::awaitable<graph::NodeOutput> run(graph::NodeInput in) override {
            EXPECT_NE(in.ctx.thread_id.find("subgraph/"), std::string::npos);
            // Raise the run fence (owner_scope, run_id) to 2 while inside the subgraph thread.
            store_->raise_fence(kOwner, run_id_, 2);
            graph::NodeOutput out;
            out.writes.push_back(graph::ChannelWrite{"val", json("from-sub")});
            co_return out;
        }

    private:
        std::string                                     name_;
        std::shared_ptr<graph::InMemoryCheckpointStore> store_;
        std::string                                     run_id_;
    };

    graph::GraphRegistry registry;
    registry.register_type("t4.sub_step",
                           [store, run_id](const std::string& name, const json&, const graph::NodeContext&) {
                               return std::make_unique<SubFenceNode>(name, store, run_id);
                           });

    const json child_def{
        {"name", "child"},
        {"channels", json{{"val", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"sub_step", json{{"type", "t4.sub_step"}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "sub_step"}},
                               json{{"from", "sub_step"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()}};

    const json parent_def{
        {"name", "parent"},
        {"channels", json{{"val", json{{"reducer", "overwrite"}, {"initial", ""}}}}},
        {"nodes", json{{"sub", json{{"type", "subgraph"}, {"definition", child_def}}}}},
        {"edges", json::array({json{{"from", "__start__"}, {"to", "sub"}},
                               json{{"from", "sub"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()}};

    graph::EngineConfig engine_cfg;
    engine_cfg.checkpoint_store = fenced;
    engine_cfg.stderr_notices   = false;
    graph::EngineResources resources;
    resources.registry = registry.snapshot();
    auto parent_engine = graph::GraphEngine::build(parent_def, std::move(engine_cfg), std::move(resources));

    graph::RunConfig run_cfg;
    run_cfg.thread_id = "root-thread-1";
    graph::RunMetadata meta;
    meta.owner_scope = kOwner;
    meta.run_id      = run_id;
    try {
        (void)parent_engine->run(run_cfg, meta);
        FAIL() << "subgraph write with stale run fence wf = 1 must throw CheckpointWriteFenceError";
    } catch (const graph::CheckpointWriteFenceError& err) {
        EXPECT_EQ(err.refusal().owner_scope, kOwner);
        EXPECT_EQ(err.refusal().run_id, run_id);
        EXPECT_EQ(err.refusal().writer_fence, 1U);
        EXPECT_EQ(err.refusal().stored_fence, 2U);
    }
}

TEST(ProgramRootLeaseTakeover, T6a_AbortedTakeoverFenceRaiseTriggersSelfTakeoverAndCompletesRun) {
    SharedBackend backend;
    Process       original(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    // Keep the shared checkpoint store at fence = 1 and inject a single CheckpointWriteFenceError(writer=1, stored=2)
    // on Attempt 1's next checkpoint write, so that self_takeover itself must raise the checkpoint store fence from 1 to 2
    // before its takeover CAS and before the successor attempt starts.
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 1U);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
    bool observed_raised_fence_before_cas = false;
    original.view->before_takeover = [&] {
        observed_raised_fence_before_cas = true;
        EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U)
            << "self_takeover must raise the checkpoint store fence to 2 before the takeover CAS";
    };
    original.checkpoints->inject_refusal_stored_fence.store(2U);

    // Release Attempt 1: its next checkpoint write gets CheckpointWriteFenceError(writer=1, stored=2),
    // triggers self-takeover to fence = 2, and completes the original ProgramHandle as Completed!
    world.tail_gate.release();
    const auto result = source.wait();
    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    EXPECT_TRUE(observed_raised_fence_before_cas);
    EXPECT_EQ(original.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}));
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U);
    EXPECT_EQ(items_of(result.output()), std::set<std::string>(kSends.begin(), kSends.end()));
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "last"), 1U);
}

TEST(ProgramRootLeaseTakeover, T6b_ConcurrentRefusalsInFanOutSingleFlightOneSelfTakeoverAndNoCleanupWrites) {
    SharedBackend backend;
    Process       original(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit_parallel_three();

    // Three parallel call_core branches run concurrently, each holding its own AttemptOpGuard (active_attempt_ops_ == 3).
    // In "tail" (after each branch has already saved its "head", "fan", and "work" checkpoints at wf = 1):
    // - the 1st branch to enter "tail" is held at a gate inside the node body (world.on_tail_async);
    // - the 2nd and 3rd branches enter put_writes for "tail" together, where the store fence is raised to 2 so both
    //   fail with CheckpointWriteFenceError while in flight at once;
    // - the single-flight winner parks in drain_attempt_ops_async(1), the loser returns without an error the join records
    //   and drops its AttemptOpGuard, and the 3rd op stays at its gate until the RunControl drain seam reports that the
    //   winner is parked in the drain with only the 3rd op remaining (active_ops == 2, own_op_tokens == 1).
    std::mutex              coord_mutex;
    std::condition_variable coord_cv;
    int                     tail_entered        = 0;
    int                     arrived_tail_writes = 0;
    bool                    barrier_released    = false;
    bool                    winner_parked       = false;
    bool                    abort_waits         = false;
    std::atomic<bool>       source_done{false};
    std::atomic<bool>       third_op_released{false};
    std::atomic<bool>       successor_started_before_third_op{false};
    std::atomic<int>        self_takeover_fence_raises{0};
    std::atomic<int>        self_takeover_entries{0};
    std::atomic<int>        successor_tail_runs{0};

    struct ScopedSelfTakeoverObserver {
        static std::mutex& mutex() {
            static std::mutex m;
            return m;
        }
        static std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)>& callback() {
            static std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> cb;
            return cb;
        }
        static bool dispatch(std::string_view run_id,
                             std::uint64_t    attempt,
                             std::uint64_t    fence,
                             bool             transition_mutex_locked) noexcept {
            std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> fn;
            {
                std::lock_guard lock(mutex());
                fn = callback();
            }
            return fn ? fn(run_id, attempt, fence, transition_mutex_locked) : true;
        }
        explicit ScopedSelfTakeoverObserver(
            std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> fn) {
            {
                std::lock_guard lock(mutex());
                callback() = std::move(fn);
            }
            program::detail::set_self_takeover_entered_observer_for_testing(&ScopedSelfTakeoverObserver::dispatch);
        }
        ~ScopedSelfTakeoverObserver() {
            program::detail::set_self_takeover_entered_observer_for_testing(nullptr);
            std::lock_guard lock(mutex());
            callback() = nullptr;
        }
    };

    ScopedSelfTakeoverObserver self_takeover_observer(
        [&](std::string_view run_id, std::uint64_t, std::uint64_t, bool) {
            if (run_id == world.run_id()) {
                self_takeover_entries.fetch_add(1);
            }
            return true;
        });

    struct ScopedDrainObserver {
        static std::mutex& mutex() {
            static std::mutex m;
            return m;
        }
        static std::function<void(std::string_view, std::uint64_t, std::size_t, std::size_t)>& callback() {
            static std::function<void(std::string_view, std::uint64_t, std::size_t, std::size_t)> cb;
            return cb;
        }
        static void dispatch(std::string_view run_id,
                             std::uint64_t    attempt,
                             std::size_t      active_ops,
                             std::size_t      own_op_tokens) noexcept {
            std::function<void(std::string_view, std::uint64_t, std::size_t, std::size_t)> fn;
            {
                std::lock_guard lock(mutex());
                fn = callback();
            }
            if (fn) fn(run_id, attempt, active_ops, own_op_tokens);
        }
        explicit ScopedDrainObserver(
            std::function<void(std::string_view, std::uint64_t, std::size_t, std::size_t)> fn) {
            {
                std::lock_guard lock(mutex());
                callback() = std::move(fn);
            }
            program::detail::set_refusal_drain_parked_observer_for_testing(&ScopedDrainObserver::dispatch);
        }
        ~ScopedDrainObserver() {
            program::detail::set_refusal_drain_parked_observer_for_testing(nullptr);
            std::lock_guard lock(mutex());
            callback() = nullptr;
        }
    };

    ScopedDrainObserver drain_observer(
        [&](std::string_view run_id, std::uint64_t attempt, std::size_t active_ops, std::size_t own_op_tokens) {
            if (run_id == world.run_id() && attempt == 1U && own_op_tokens == 1U && active_ops <= 2U) {
                {
                    std::lock_guard lock(coord_mutex);
                    winner_parked = true;
                }
                coord_cv.notify_all();
            }
        });

    const ReleaseOnExit release_coord{[&] {
        {
            std::lock_guard lock(coord_mutex);
            barrier_released = true;
            winner_parked    = true;
            abort_waits      = true;
        }
        coord_cv.notify_all();
    }};

    world.on_tail_async = [&](const std::string&) -> asio::awaitable<void> {
        if (self_takeover_fence_raises.load() > 0) {
            successor_tail_runs.fetch_add(1);
        }
        bool hold_this_branch = false;
        {
            std::lock_guard lock(coord_mutex);
            if (++tail_entered == 1) {
                hold_this_branch = true;
            }
        }
        if (!hold_this_branch) co_return;
        // Hold the 3rd op until the RunControl seam reports the single-flight winner is parked in the drain
        // with only this op still active (or the run has ended).
        {
            std::unique_lock lock(coord_mutex);
            coord_cv.wait(lock, [&] { return winner_parked || source_done.load() || abort_waits; });
            EXPECT_TRUE(winner_parked);
        }
        EXPECT_EQ(self_takeover_fence_raises.load(), 0)
            << "successor control / self_takeover must not start while the 3rd op is still active";
        EXPECT_EQ(original.view->takeover_attempts.load(), 0)
            << "successor takeover must not start while the 3rd op is still active";
        EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
        third_op_released.store(true);
        co_return;
    };

    original.checkpoints->before_put_writes = [&](const graph::PendingWrite& write) {
        if (write.node_name != "tail") return;
        std::unique_lock lock(coord_mutex);
        if (barrier_released) return;
        ++arrived_tail_writes;
        if (arrived_tail_writes == 2) {
            original.checkpoints->clear_writes_calls.store(0);
            original.checkpoints->delete_thread_calls.store(0);
            backend.checkpoints->raise_fence(kOwner, world.run_id(), 2);
            barrier_released = true;
            coord_cv.notify_all();
        } else {
            coord_cv.wait(lock, [&] { return barrier_released || source_done.load() || abort_waits; });
        }
    };

    original.checkpoints->before_raise_fence = [&](std::uint64_t) {
        self_takeover_fence_raises.fetch_add(1);
        if (!third_op_released.load()) {
            successor_started_before_third_op.store(true);
        }
    };

    auto       source = original.host.start_parallel_three(version, world.run_id());
    const auto result = source.wait();
    source_done.store(true);
    coord_cv.notify_all();

    EXPECT_EQ(result.status(), ProgramTerminalStatus::Completed) << failure_of(result);
    EXPECT_EQ(result.attempt(), 1U);
    EXPECT_EQ(successor_tail_runs.load(), 3)
        << "successor control at fence 2 must start after drain and execute all 3 branches";
    EXPECT_TRUE(third_op_released.load());
    EXPECT_FALSE(successor_started_before_third_op.load())
        << "drain_attempt_ops_async(1) must wait until the 3rd op releases its AttemptOpGuard before self_takeover runs";
    EXPECT_EQ(self_takeover_entries.load(), 1)
        << "single-flight refusal claim must allow only one refusal handler into self_takeover";
    EXPECT_EQ(self_takeover_fence_raises.load(), 1)
        << "single-flight refusal claim must allow only one winner into self_takeover";
    EXPECT_EQ(items_of(result.output()), std::set<std::string>(kSends.begin(), kSends.end()));
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_EQ(original.view->published_takeovers.load(), 1)
        << "single-flight winner performs exactly one self-takeover while loser drops its op cleanly";
    EXPECT_EQ(original.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}));
    EXPECT_EQ(original.checkpoints->clear_writes_calls.load(), 0);
    EXPECT_EQ(original.checkpoints->delete_thread_calls.load(), 0);

    // A late wf = 1 write after retire_to is refused by the store without triggering a second takeover.
    graph::FencedCheckpointStore stale_writer(backend.checkpoints, kOwner, world.run_id(), 1);
    graph::Checkpoint            late_cp;
    late_cp.id             = graph::Checkpoint::generate_id();
    late_cp.thread_id      = original.checkpoints->saved_thread();
    late_cp.channel_values = json::object();
    late_cp.step           = 77;
    late_cp.timestamp      = 1;
    EXPECT_THROW(stale_writer.save(late_cp), graph::CheckpointWriteFenceError);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
}

TEST(ProgramRootLeaseTakeover, T7a_TakeoverRetriesCasOnJournalHeadConflictAtSameFence) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    Gate          before_sp, after_sp;
    before_sp.arm();
    after_sp.arm();
    original.view->before_safe_point = [&] { before_sp.pass(); };
    original.view->after_safe_point  = [&] { after_sp.pass(); };
    world.fan_gate.arm();

    auto              source = original.host.start(version, world.run_id());
    std::atomic<bool> source_done{false};
    auto              source_end = std::async(std::launch::async, [&] {
        const auto result = source.wait();
        source_done.store(true);
        before_sp.notify();
        after_sp.notify();
        return result;
    });
    std::optional<MigrationRace> race;
    race.emplace(original, world, version, source);
    const ReleaseOnExit release_all{[&] {
        before_sp.release();
        after_sp.release();
        world.fan_gate.release();
    }};
    ASSERT_TRUE(before_sp.wait_entered_or([&] { return source_done.load(); }))
        << "the source ended before the migration reached a safe point";

    const auto initial_record = backend.transitions->load_sync(kOwner, world.run_id());
    ASSERT_TRUE(initial_record.has_value());
    const auto initial_head = initial_record->journal_head();

    // Between recovering's single raise_fence(2) and its first takeover CAS, release original's safe-point
    // publication so the stored run record's journal_head genuinely advances at fence = 1 in the shared store.
    recovering.view->before_takeover = [&] {
        if (recovering.view->takeover_attempts.load() == 1) {
            EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U);
            EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
            before_sp.release();
            ASSERT_TRUE(after_sp.wait_entered_or([&] { return source_done.load(); }));
            const auto advanced = backend.transitions->load_sync(kOwner, world.run_id());
            ASSERT_TRUE(advanced.has_value());
            EXPECT_EQ(advanced->continuation().fence, 1U);
            EXPECT_NE(advanced->journal_head(), initial_head);
        }
    };

    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered);
    EXPECT_EQ(recovering.view->takeover_attempts.load(), 2)
        << "reconnect_core must retry takeover CAS after a same-fence journal_head conflict";
    EXPECT_EQ(recovering.view->published_takeovers.load(), 1);
    EXPECT_EQ(recovering.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}))
        << "reconnect_core must not call raise_fence a second time on CAS retry";
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);

    after_sp.release();
    EXPECT_FALSE(race->migration.get()) << "a migration of a source that was taken over published a successor";
    const auto stale = source_end.get();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
    race.reset();
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "tail"), 1U);
    EXPECT_EQ(runs_of(runs, "last"), 1U);
}

TEST(ProgramRootLeaseTakeover, T7a_ProgressGuardRefusesWhenConflictReloadShowsSameHeadAndFence) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    // Force the takeover CAS to return Conflict without advancing journal_head or fence in the shared store.
    // The progress guard in reconnect_core must refuse with P_RUN_RECOVERY_CONFLICT after 1 attempt without looping.
    recovering.view->force_takeover_conflict.store(true);
    try {
        (void)recovering.host.reconnect(world.run_id());
        FAIL() << "reconnect_core must refuse with P_RUN_RECOVERY_CONFLICT when reload shows unchanged head and fence";
    } catch (const ProgramDiagnosticError& err) {
        EXPECT_EQ(err.diagnostic().code, "P_RUN_RECOVERY_CONFLICT");
    }
    EXPECT_EQ(recovering.view->takeover_attempts.load(), 1);
    EXPECT_EQ(recovering.view->published_takeovers.load(), 0);
    EXPECT_EQ(recovering.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}));
    EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);

    // Clear the injected conflict; a subsequent reconnect takes over to fence = 2 and finishes the run.
    recovering.view->force_takeover_conflict.store(false);
    const auto recovered = recovering.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(recovered.status(), ProgramTerminalStatus::Completed) << failure_of(recovered);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);

    world.tail_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
    ASSERT_TRUE(stale.failure());
    EXPECT_EQ(stale.failure()->code, "P_RUN_TAKEN_OVER");
}

TEST(ProgramRootLeaseTakeover, T7b_ConcurrentForeignReconnectLoserDoesNotRaiseTwiceOrPublish) {
    SharedBackend backend;
    Process       original(backend), winner(backend), loser(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    // Hold loser right before its compare_publish_takeover_execution (after loser has called raise_fence(2) once).
    Gate loser_at_cas;
    loser_at_cas.arm();
    const ReleaseOnExit release_loser{[&] { loser_at_cas.release(); }};
    loser.view->before_takeover = [&] {
        loser_at_cas.pass();
    };

    std::atomic<bool> loser_done{false};
    auto loser_future = std::async(std::launch::async, [&] {
        const auto res = loser.host.reconnect(world.run_id()).wait();
        loser_done.store(true);
        loser_at_cas.notify();
        return res;
    });
    ASSERT_TRUE(loser_at_cas.wait_entered_or([&] { return loser_done.load(); }));

    // Winner completes takeover to fence = 2 and finishes the run.
    const auto won = winner.host.reconnect(world.run_id()).wait();
    ASSERT_EQ(won.status(), ProgramTerminalStatus::Completed) << failure_of(won);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);

    // Release loser: its CAS conflicts, it reloads r_fence = 2 > expected_fence = 1, and exits without a second raise.
    loser_at_cas.release();
    const auto lost = loser_future.get();
    EXPECT_EQ(lost.status(), ProgramTerminalStatus::Failed) << failure_of(lost);
    ASSERT_TRUE(lost.failure());
    EXPECT_EQ(lost.failure()->code, "P_RUN_TAKEN_OVER");

    // Binding Correction #3: allow the loser's one pre-loop fence upsert to expected_fence + 1 (2);
    // assert no second raise, no transition publish, and no checkpoint rows from the loser.
    EXPECT_EQ(loser.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}));
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_EQ(loser.view->published_takeovers.load(), 0);
    EXPECT_EQ(loser.view->published_transitions.load(), 0);
    EXPECT_EQ(loser.checkpoints->saves.load(), 0);
    EXPECT_EQ(loser.checkpoints->put_writes_calls.load(), 0);

    world.tail_gate.release();
    const auto stale = source.wait();
    EXPECT_EQ(stale.status(), ProgramTerminalStatus::Failed) << failure_of(stale);
}

TEST(ProgramRootLeaseTakeover, T7c_RaiseThenOwnerPublishesInterruptedAdvancesInterruptedFenceBeforeResume) {
    SharedBackend backend;
    Process       original(backend), recovering(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.interrupt_tail.store(true);

    // Hold original right before it publishes ContinuationState::Interrupted at fence = 1
    // (after original has already saved the NodeInterrupt checkpoint at wf = 1).
    Gate original_at_interrupt_pub;
    original_at_interrupt_pub.arm();
    const ReleaseOnExit release_interrupt_pub{[&] { original_at_interrupt_pub.release(); }};
    original.view->before_compare_publish = [&](const ProgramTransitionPublication& pub) {
        if (pub.run_record.continuation().state == ContinuationState::Interrupted) {
            original_at_interrupt_pub.pass();
        }
    };

    auto source = original.host.start(version, world.run_id());
    ASSERT_TRUE(original_at_interrupt_pub.wait_entered_or([&] { return static_cast<bool>(source.try_result()); }));

    // Recovering starts reconnect: right after raise_fence(2) commits, let original publish Interrupted at fence = 1.
    std::atomic<bool> let_original_publish{false};
    recovering.checkpoints->after_raise_fence = [&](std::uint64_t raised) {
        EXPECT_EQ(raised, 2U);
        if (!let_original_publish.exchange(true)) {
            original_at_interrupt_pub.release();
            const auto paused = source.wait();
            EXPECT_EQ(paused.status(), ProgramTerminalStatus::Interrupted) << failure_of(paused);
            EXPECT_EQ(stored_fence(backend, world.run_id()), 1U);
        }
    };

    // Recovering's first takeover CAS conflicts (head moved to Interrupted at fence = 1);
    // it reloads Interrupted at fence = 1, CASes the Interrupted record's fence to 2, and returns the Interrupted handle.
    auto reconnected = recovering.host.reconnect(world.run_id());
    const auto paused_after_takeover = reconnected.wait();
    EXPECT_EQ(paused_after_takeover.status(), ProgramTerminalStatus::Interrupted)
        << failure_of(paused_after_takeover);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U)
        << "reconnect_core must advance the Interrupted record's fence to 2 before returning the handle (G3)";
    EXPECT_EQ(recovering.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}));

    // Resume on recovering: the resumed attempt is pinned to fence = 2, so its later saves (wf = 2 >= K.sf = 2) succeed!
    ASSERT_TRUE(paused_after_takeover.interrupt().has_value());
    ASSERT_TRUE(paused_after_takeover.interrupt()->pending_input.has_value());
    const auto pending_call_id = paused_after_takeover.interrupt()->pending_input->call_id();
    const auto resumed =
        recovering.host.runtime().resume(
            kOwner, world.run_id(),
            ProgramResume{json{{"decision", "approved"}}, "trace-resume", {}, pending_call_id}).wait();
    EXPECT_EQ(resumed.status(), ProgramTerminalStatus::Completed) << failure_of(resumed);
    EXPECT_EQ(stored_fence(backend, world.run_id()), 2U);
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world.run_id()), 2U);
    EXPECT_EQ(recovering.checkpoints->last_saved_fence(), 2U)
        << "resumed attempt must save its checkpoints with writer fence = 2";
    const auto latest = backend.checkpoints->load_latest(original.checkpoints->saved_thread());
    ASSERT_TRUE(latest.has_value());
    ASSERT_TRUE(latest->channel_values.contains("channels"));
    EXPECT_TRUE(channel_is_set(latest->channel_values["channels"], "finished"))
        << "checkpoint written by the resumed attempt at fence 2 must be readable from the store";
    const auto runs = take_runs(world);
    EXPECT_EQ(runs_of(runs, "last"), 1U);

    // Also verify direct resume() (without calling reconnect() first) after an aborted raise_fence(2) left the
    // checkpoint store at fence = 2 while the Interrupted transition record remained at fence = 1.
    LeaseWorld world_direct(next_world_id());
    world_direct.interrupt_tail.store(true);
    const auto paused_direct = original.host.start(version, world_direct.run_id()).wait();
    ASSERT_EQ(paused_direct.status(), ProgramTerminalStatus::Interrupted) << failure_of(paused_direct);
    ASSERT_TRUE(paused_direct.interrupt().has_value());
    ASSERT_TRUE(paused_direct.interrupt()->pending_input.has_value());
    EXPECT_EQ(stored_fence(backend, world_direct.run_id()), 1U);
    EXPECT_EQ(backend.checkpoints->raise_fence(kOwner, world_direct.run_id(), 2), 2U);

    // Clear the pre-pause step journal and observe any takeover published after the resumed step starts.
    (void)take_runs(world_direct);
    std::atomic<bool> resumed_tail_started{false};
    std::atomic<int>  takeovers_after_resumed_step{0};
    world_direct.on_tail_async = [&](const std::string&) -> asio::awaitable<void> {
        resumed_tail_started.store(true);
        co_return;
    };
    recovering.view->before_takeover = [&] {
        if (resumed_tail_started.load()) {
            takeovers_after_resumed_step.fetch_add(1);
        }
    };

    const auto direct_call_id = paused_direct.interrupt()->pending_input->call_id();
    const auto resumed_direct =
        recovering.host.runtime().resume(
            kOwner, world_direct.run_id(),
            ProgramResume{json{{"decision", "approved"}}, "trace-resume-direct", {}, direct_call_id}).wait();
    EXPECT_EQ(resumed_direct.status(), ProgramTerminalStatus::Completed) << failure_of(resumed_direct);
    EXPECT_EQ(resumed_direct.attempt(), 2U)
        << "direct resume must advance the Interrupted fence before starting attempt 2 rather than failing a write into attempt 3";
    const auto runs_direct = take_runs(world_direct);
    EXPECT_EQ(runs_of(runs_direct, "tail"), 1U)
        << "the resumed step after the pause must run exactly once without failing its write and replaying via self-takeover";
    EXPECT_EQ(runs_of(runs_direct, "last"), 1U);
    EXPECT_EQ(takeovers_after_resumed_step.load(), 0)
        << "no self-takeover may be published after the resumed attempt starts";
    EXPECT_EQ(recovering.checkpoints->raised_fences(), (std::vector<std::uint64_t>{2U}))
        << "direct resume must not trigger a self-takeover raise_fence on recovering";
    EXPECT_EQ(stored_fence(backend, world_direct.run_id()), 2U);
    EXPECT_EQ(backend.checkpoints->load_fence(kOwner, world_direct.run_id()), 2U);
    EXPECT_EQ(recovering.checkpoints->last_saved_fence(), 2U);
    const auto latest_direct = backend.checkpoints->load_latest(recovering.checkpoints->saved_thread());
    ASSERT_TRUE(latest_direct.has_value());
    ASSERT_TRUE(latest_direct->channel_values.contains("channels"));
    EXPECT_TRUE(channel_is_set(latest_direct->channel_values["channels"], "finished"));
}

TEST(ProgramRootLeaseTakeover, T8a_UnfencedCustomCheckpointStoreAllowsInitialRunAndRefusesTakeover) {
    class UnfencedCheckpointStore final : public graph::CheckpointStore {
    public:
        explicit UnfencedCheckpointStore(std::shared_ptr<graph::CheckpointStore> inner)
            : inner_(std::move(inner)) {}
        bool supports_write_fence() const override { return false; }
        bool requires_managed_budget(const std::string& t) override {
            return inner_->requires_managed_budget(t);
        }
        void save(const graph::Checkpoint& cp) override { inner_->save(cp); }
        std::optional<graph::Checkpoint> load_latest(const std::string& t) override {
            return inner_->load_latest(t);
        }
        std::optional<graph::Checkpoint> load_by_id(const std::string& id) override {
            return inner_->load_by_id(id);
        }
        std::vector<graph::Checkpoint> list(const std::string& t, int limit) override {
            return inner_->list(t, limit);
        }
        void delete_thread(const std::string& t) override { inner_->delete_thread(t); }
        void put_writes(const std::string& t, const std::string& p, const graph::PendingWrite& w) override {
            inner_->put_writes(t, p, w);
        }
        std::vector<graph::PendingWrite> get_writes(const std::string& t, const std::string& p) override {
            return inner_->get_writes(t, p);
        }
        void clear_writes(const std::string& t, const std::string& p) override {
            inner_->clear_writes(t, p);
        }

    private:
        std::shared_ptr<graph::CheckpointStore> inner_;
    };

    SharedBackend backend;
    auto          unfenced = std::make_shared<UnfencedCheckpointStore>(backend.checkpoints);
    LeaseHost     host1(ProcessStores{backend.programs, std::make_shared<ProcessView>(backend.transitions), unfenced});
    LeaseHost     host2(ProcessStores{backend.programs, std::make_shared<ProcessView>(backend.transitions), unfenced});

    // 1. Initial run (fence = 1) succeeds on an unfenced store (R4).
    LeaseWorld world1(next_world_id());
    const auto version = host1.admit();
    const auto initial = host1.run(version, world1.run_id());
    EXPECT_EQ(initial.status(), ProgramTerminalStatus::Completed) << failure_of(initial);

    // 2. Takeover on an unfenced store fails closed with P_RUN_TAKEOVER_FENCE_UNSUPPORTED.
    LeaseWorld world2(next_world_id());
    world2.tail_gate.arm();
    const ReleaseOnExit release_tail2{[&] { world2.tail_gate.release(); }};
    auto running = host1.start(version, world2.run_id());
    ASSERT_TRUE(world2.tail_gate.wait_entered_or([&] { return static_cast<bool>(running.try_result()); }));

    try {
        (void)host2.reconnect(world2.run_id());
        FAIL() << "reconnect on a CheckpointStore with supports_write_fence() == false must fail closed";
    } catch (const ProgramDiagnosticError& err) {
        EXPECT_EQ(err.diagnostic().code, "P_RUN_TAKEOVER_FENCE_UNSUPPORTED");
    }

    world2.tail_gate.release();
    const auto finished = running.wait();
    EXPECT_EQ(finished.status(), ProgramTerminalStatus::Completed) << failure_of(finished);
}

TEST(ProgramRootLeaseTakeover, ReconnectDoesNotHangWhileControlIsInRefusal) {
    struct ScopedControlWaitObserver {
        static std::mutex& mutex() {
            static std::mutex m;
            return m;
        }
        static std::function<void(std::string_view, std::uint64_t, bool)>& callback() {
            static std::function<void(std::string_view, std::uint64_t, bool)> cb;
            return cb;
        }
        static void dispatch(std::string_view run_id,
                             std::uint64_t    attempt,
                             bool             transition_mutex_locked) noexcept {
            std::function<void(std::string_view, std::uint64_t, bool)> fn;
            {
                std::lock_guard lock(mutex());
                fn = callback();
            }
            if (fn) fn(run_id, attempt, transition_mutex_locked);
        }
        explicit ScopedControlWaitObserver(
            std::function<void(std::string_view, std::uint64_t, bool)> fn) {
            {
                std::lock_guard lock(mutex());
                callback() = std::move(fn);
            }
            program::detail::set_control_wait_parked_observer_for_testing(&ScopedControlWaitObserver::dispatch);
        }
        ~ScopedControlWaitObserver() {
            program::detail::set_control_wait_parked_observer_for_testing(nullptr);
            std::lock_guard lock(mutex());
            callback() = nullptr;
        }
    };

    struct ScopedSelfTakeoverGuard {
        static std::mutex& mutex() {
            static std::mutex m;
            return m;
        }
        static std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)>& callback() {
            static std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> cb;
            return cb;
        }
        static bool dispatch(std::string_view run_id,
                             std::uint64_t    attempt,
                             std::uint64_t    fence,
                             bool             transition_mutex_locked) noexcept {
            std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> fn;
            {
                std::lock_guard lock(mutex());
                fn = callback();
            }
            return fn ? fn(run_id, attempt, fence, transition_mutex_locked) : true;
        }
        explicit ScopedSelfTakeoverGuard(
            std::function<bool(std::string_view, std::uint64_t, std::uint64_t, bool)> fn) {
            {
                std::lock_guard lock(mutex());
                callback() = std::move(fn);
            }
            program::detail::set_self_takeover_entered_observer_for_testing(&ScopedSelfTakeoverGuard::dispatch);
        }
        ~ScopedSelfTakeoverGuard() {
            program::detail::set_self_takeover_entered_observer_for_testing(nullptr);
            std::lock_guard lock(mutex());
            callback() = nullptr;
        }
    };

    SharedBackend backend;
    Process       original(backend), pausing(backend);
    LeaseWorld    world(next_world_id());
    const auto    version = original.host.admit();
    world.tail_gate.arm();
    const ReleaseOnExit release_tail{[&] { world.tail_gate.release(); }};

    // Part 1: self_takeover -> reconnect_core(..., transition_lock_held=true) waiting on an existing unfinished control.
    // Start control_a (attempt = 1, fence = 1) on original and hold it in "tail".
    auto source_a = original.host.start(version, world.run_id());
    ASSERT_TRUE(world.tail_gate.wait_entered_or([&] { return static_cast<bool>(source_a.try_result()); }));

    Gate                         tail_b_entered;
    tail_b_entered.arm();
    std::optional<ProgramHandle> resumed_b;
    std::atomic<bool>            lineage_hook_ran{false};
    std::atomic<bool>            b_entered_put_writes{false};
    std::atomic<bool>            b_wait_parked{false};
    std::atomic<bool>            b_wait_parked_while_locked{false};
    std::atomic<bool>            b_self_takeover_locked{false};
    const ReleaseOnExit          release_b_gates{[&] {
        tail_b_entered.release();
    }};

    ScopedControlWaitObserver wait_observer_b(
        [&](std::string_view run_id, std::uint64_t attempt, bool transition_mutex_locked) {
            if (run_id == world.run_id() && attempt == 2U) {
                b_wait_parked.store(true);
                b_wait_parked_while_locked.store(transition_mutex_locked);
                tail_b_entered.release();
            }
        });

    ScopedSelfTakeoverGuard takeover_guard_b(
        [&](std::string_view run_id, std::uint64_t attempt, std::uint64_t, bool transition_mutex_locked) {
            if (run_id == world.run_id() && attempt == 2U) {
                b_self_takeover_locked.store(transition_mutex_locked);
                return !transition_mutex_locked && b_wait_parked.load() && !b_wait_parked_while_locked.load();
            }
            return true;
        });

    original.checkpoints->before_put_writes = [&](const graph::PendingWrite& write) {
        if (write.node_name == "tail" && lineage_hook_ran.load() && !b_entered_put_writes.exchange(true)) {
            tail_b_entered.pass_or([&] {
                return static_cast<bool>(source_a.try_result()) ||
                       (resumed_b.has_value() && static_cast<bool>(resumed_b->try_result()));
            });
        }
    };

    original.view->on_run_lineage_read = [&] {
        if (lineage_hook_ran.exchange(true)) return;
        world.interrupt_tail.store(true);
        const auto paused_2 = pausing.host.reconnect(world.run_id()).wait();
        ASSERT_EQ(paused_2.status(), ProgramTerminalStatus::Interrupted) << failure_of(paused_2);
        ASSERT_TRUE(paused_2.interrupt().has_value());
        ASSERT_TRUE(paused_2.interrupt()->pending_input.has_value());

        world.interrupt_tail.store(false);
        const auto call_id_2 = paused_2.interrupt()->pending_input->call_id();
        resumed_b.emplace(original.host.runtime().resume(
            kOwner, world.run_id(),
            ProgramResume{json{{"decision", "approved"}}, "trace-resume-b", {}, call_id_2}));
        ASSERT_TRUE(tail_b_entered.wait_entered_or([&] {
            return static_cast<bool>(resumed_b->try_result()) || static_cast<bool>(source_a.try_result());
        }));

        // Advance the durable record to ContinuationState::Interrupted at fence = 3 while control_b (fence = 2) is
        // still live, unfinished, and has not claimed a write-fence refusal yet. Because resume persisted an
        // attempt_resume for "tail", pausing's takeover replays "tail" with that resume value and then interrupts
        // at "last".
        world.interrupt_last.store(true);
        const auto paused_3 = pausing.host.reconnect(world.run_id()).wait();
        ASSERT_EQ(paused_3.status(), ProgramTerminalStatus::Interrupted) << failure_of(paused_3);
        EXPECT_EQ(stored_fence(backend, world.run_id()), 3U);
    };

    original.checkpoints->inject_refusal_stored_fence.store(2U);
    world.tail_gate.release();

    const auto result_a = source_a.wait();
    tail_b_entered.release();
    ASSERT_TRUE(resumed_b.has_value());
    const auto result_b = resumed_b->wait();
    EXPECT_TRUE(b_wait_parked.load());
    EXPECT_FALSE(b_wait_parked_while_locked.load());
    EXPECT_FALSE(b_self_takeover_locked.load());
    EXPECT_EQ(result_a.status(), ProgramTerminalStatus::Failed) << failure_of(result_a);
    ASSERT_TRUE(result_a.failure().has_value());
    EXPECT_EQ(result_a.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(result_b.status(), ProgramTerminalStatus::Failed) << failure_of(result_b);
    ASSERT_TRUE(result_b.failure().has_value());
    EXPECT_EQ(result_b.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(stored_fence(backend, world.run_id()), 3U);

    // Part 2: Public top-level reconnect(run_id) wrapper path (transition_lock_held=false) on an Interrupted run
    // with a live, unfinished control on the same process.
    original.view->on_run_lineage_read = nullptr;
    original.checkpoints->inject_refusal_stored_fence.store(0U);
    LeaseWorld world_top(next_world_id());
    Gate       tail_top_entered;
    tail_top_entered.arm();
    std::atomic<bool> top_entered_put_writes{false};
    std::atomic<bool> top_wait_parked{false};
    std::atomic<bool> top_wait_parked_while_locked{false};
    std::atomic<bool> top_self_takeover_locked{false};
    std::atomic<bool> top_reconnect_done{false};
    const ReleaseOnExit release_top_gates{[&] {
        tail_top_entered.release();
    }};

    ScopedControlWaitObserver wait_observer_top(
        [&](std::string_view run_id, std::uint64_t attempt, bool transition_mutex_locked) {
            if (run_id == world_top.run_id() && attempt == 1U) {
                top_wait_parked.store(true);
                top_wait_parked_while_locked.store(transition_mutex_locked);
                tail_top_entered.release();
            }
        });

    ScopedSelfTakeoverGuard takeover_guard_top(
        [&](std::string_view run_id, std::uint64_t attempt, std::uint64_t, bool transition_mutex_locked) {
            if (run_id == world_top.run_id() && attempt == 1U) {
                top_self_takeover_locked.store(transition_mutex_locked);
                return !transition_mutex_locked && top_wait_parked.load() && !top_wait_parked_while_locked.load();
            }
            return true;
        });

    std::optional<ProgramHandle> source_top;
    original.checkpoints->before_put_writes = [&](const graph::PendingWrite& write) {
        if (write.node_name == "tail" && !top_entered_put_writes.exchange(true)) {
            tail_top_entered.pass_or([&] {
                return top_reconnect_done.load() ||
                       (source_top.has_value() && static_cast<bool>(source_top->try_result()));
            });
        }
    };

    source_top.emplace(original.host.start(version, world_top.run_id()));
    ASSERT_TRUE(tail_top_entered.wait_entered_or([&] { return static_cast<bool>(source_top->try_result()); }));

    // pausing takes over to fence = 2 and publishes ContinuationState::Interrupted while source_top (fence = 1) is
    // still live, unfinished, and parked in before_put_writes on original.
    world_top.interrupt_tail.store(true);
    const auto paused_top = pausing.host.reconnect(world_top.run_id()).wait();
    ASSERT_EQ(paused_top.status(), ProgramTerminalStatus::Interrupted) << failure_of(paused_top);
    EXPECT_EQ(stored_fence(backend, world_top.run_id()), 2U);

    // Calling the public top-level original.host.reconnect(world_top.run_id()) finds the Interrupted record at fence = 2
    // and the live, unfinished source_top control on original, unlocks reconnect_lock before awaiting
    // handle.control_->wait_async(), and lets source_top finish its refusal self_takeover without deadlocking.
    ProgramHandle reconnected_top = original.host.reconnect(world_top.run_id());
    top_reconnect_done.store(true);
    tail_top_entered.release();

    const auto result_reconnected_top = reconnected_top.wait();
    const auto result_source_top      = source_top->wait();
    EXPECT_TRUE(top_wait_parked.load());
    EXPECT_FALSE(top_wait_parked_while_locked.load());
    EXPECT_FALSE(top_self_takeover_locked.load());
    EXPECT_EQ(result_reconnected_top.status(), ProgramTerminalStatus::Failed) << failure_of(result_reconnected_top);
    ASSERT_TRUE(result_reconnected_top.failure().has_value());
    EXPECT_EQ(result_reconnected_top.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(result_source_top.status(), ProgramTerminalStatus::Failed) << failure_of(result_source_top);
    ASSERT_TRUE(result_source_top.failure().has_value());
    EXPECT_EQ(result_source_top.failure()->code, "P_RUN_TAKEN_OVER");
    EXPECT_EQ(stored_fence(backend, world_top.run_id()), 2U);
}

}  // namespace
