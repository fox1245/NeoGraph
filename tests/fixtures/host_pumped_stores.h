// Program stores that complete only when a host's event loop is pumped (#556, #557).
//
// Each store wraps a synchronous one and awaits one turn of the loop that runs the awaiting coroutine before it
// forwards the operation, so the call suspends and a later task of the host's executor resumes it. A thread that waits
// for such a call can never finish it; it needs a caller that awaits. The tests drive a Program lifecycle over all five
// stores to show that no step of it waits on a store or is refused for not being able to.
//
// HostTurns is the switch they share: a store completes at once while it is not pumped, so a test can start a run over
// plain stores and let the stores take turns from a given point, and it counts what each store was asked.
#pragma once

#include <neograph/program/core_tool_grant_store.h>
#include <neograph/program/module.h>
#include <neograph/program/store.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>

#include <asio/awaitable.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace neograph::program::test_support {

class HostTurns {
public:
    /// The thread that pumps the host's loop; a call that completes on another thread is counted by off_thread().
    explicit HostTurns(bool pumped = true, std::thread::id host = std::this_thread::get_id())
        : pumped_(pumped), host_(host) {}

    void set_pumped(bool pumped) { pumped_.store(pumped); }
    bool pumped() const { return pumped_.load(); }

    /// The next call of `name` ("transition.compare_publish", "program.load"...) throws instead of reaching the store, once.
    void fail_next(std::string name) {
        std::lock_guard lock(mutex_);
        fail_next_ = std::move(name);
    }

    /// Awaited first by every operation of a wrapper: suspends until the host's next turn while the stores are pumped.
    asio::awaitable<void> turn(const char* store, const char* operation) {
        {
            std::lock_guard lock(mutex_);
            const auto name = std::string(store) + "." + operation;
            ++operations_[name];
            if (!fail_next_.empty() && fail_next_ == name) {
                fail_next_.clear();
                throw std::runtime_error("injected store failure: " + name);
            }
        }
        if (!pumped_.load()) co_return;
        ++suspended_;
        co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
        if (std::this_thread::get_id() != host_) ++off_thread_;
    }

    /// Calls made to one store (by its name: "transition", "program", "grant", "fragment", "module"), or to all.
    std::uint64_t operations(std::string_view store = {}) const {
        std::lock_guard lock(mutex_);
        std::uint64_t   total = 0;
        for (const auto& [name, count] : operations_)
            if (store.empty() || (name.size() > store.size() && name.compare(0, store.size(), store) == 0 &&
                                  name[store.size()] == '.'))
                total += count;
        return total;
    }
    std::uint64_t operation(const std::string& name) const {
        std::lock_guard lock(mutex_);
        const auto      found = operations_.find(name);
        return found == operations_.end() ? 0 : found->second;
    }
    /// How many calls suspended for a turn of the host, and how many were resumed by a thread other than the host's.
    std::uint64_t suspended() const { return suspended_.load(); }
    std::uint64_t off_thread() const { return off_thread_.load(); }

private:
    std::atomic<bool>                  pumped_;
    std::thread::id                    host_;
    mutable std::mutex                 mutex_;
    std::map<std::string, std::uint64_t> operations_;
    std::string                        fail_next_;
    std::atomic<std::uint64_t>         suspended_{0};
    std::atomic<std::uint64_t>         off_thread_{0};
};

/// Not a Sync...Store: a view over it that waits for it is refused inside a turn of the host's loop.
class HostPumpedTransitionStore final : public ProgramTransitionStore {
public:
    HostPumpedTransitionStore(std::shared_ptr<ProgramTransitionStore> inner, std::shared_ptr<HostTurns> turns)
        : inner_(std::move(inner)), turns_(std::move(turns)) {}

    asio::awaitable<std::optional<ProgramRunRecord>> load(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load");
        co_return co_await inner_->load(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramJournalRecord>> latest(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "latest");
        co_return co_await inner_->latest(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunPublicationHead>> load_run_publication_head(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_run_publication_head");
        co_return co_await inner_->load_run_publication_head(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramEvent>> load_events(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await turns_->turn("transition", "load_events");
        co_return co_await inner_->load_events(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<ProgramEffectOutboxEntry>> load_effects(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await turns_->turn("transition", "load_effects");
        co_return co_await inner_->load_effects(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<ProgramJavaScriptCommandJournalEntry>> load_javascript_commands(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await turns_->turn("transition", "load_javascript_commands");
        co_return co_await inner_->load_javascript_commands(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::optional<ProgramCommandPublicationHead>> load_command_publication_head(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_command_publication_head");
        co_return co_await inner_->load_command_publication_head(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramContextPublication>> load_context_publications(std::string_view owner_scope, std::string_view run_id, std::uint64_t after_sequence) const override {
        co_await turns_->turn("transition", "load_context_publications");
        co_return co_await inner_->load_context_publications(owner_scope, run_id, after_sequence);
    }
    asio::awaitable<std::vector<HookOutboxEntry>> load_hook_outbox_entries(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_hook_outbox_entries");
        co_return co_await inner_->load_hook_outbox_entries(owner_scope, run_id);
    }
    asio::awaitable<ProgramEffectiveRuntimeState> load_effective_runtime_state(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_effective_runtime_state");
        co_return co_await inner_->load_effective_runtime_state(owner_scope, run_id);
    }
    asio::awaitable<std::vector<ProgramChildSynthesisRecord>> load_child_syntheses(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_child_syntheses");
        co_return co_await inner_->load_child_syntheses(owner_scope, run_id);
    }
    asio::awaitable<std::optional<MigrationPlan>> load_migration_plan(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_migration_plan");
        co_return co_await inner_->load_migration_plan(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_lineage(std::string_view owner_scope, std::string_view lineage_id) const override {
        co_await turns_->turn("transition", "load_lineage");
        co_return co_await inner_->load_lineage(owner_scope, lineage_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_run_lineage(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_run_lineage");
        co_return co_await inner_->load_run_lineage(owner_scope, run_id);
    }
    asio::awaitable<std::optional<ProgramRunLineage>> load_lineage_head(std::string_view owner_scope, std::string_view lineage_id, std::string_view head_id) const override {
        co_await turns_->turn("transition", "load_lineage_head");
        co_return co_await inner_->load_lineage_head(owner_scope, lineage_id, head_id);
    }
    asio::awaitable<std::optional<ProgramRunGeneration>> load_generation(std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        co_await turns_->turn("transition", "load_generation");
        co_return co_await inner_->load_generation(owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<ProgramTransitionPublication>> load_generation_initial_publication(std::string_view owner_scope, std::string_view lineage_id, std::uint64_t generation) const override {
        co_await turns_->turn("transition", "load_generation_initial_publication");
        co_return co_await inner_->load_generation_initial_publication(owner_scope, lineage_id, generation);
    }
    asio::awaitable<std::optional<GraphMigrationCapsule>> load_graph_migration_capsule(std::string_view owner_scope, std::string_view source_run_id, std::string_view source_lineage_head_id) const override {
        co_await turns_->turn("transition", "load_graph_migration_capsule");
        co_return co_await inner_->load_graph_migration_capsule(owner_scope, source_run_id, source_lineage_head_id);
    }
    asio::awaitable<std::optional<ProgramExecutionLease>> load_execution_lease(std::string_view owner_scope, std::string_view run_id) const override {
        co_await turns_->turn("transition", "load_execution_lease");
        co_return co_await inner_->load_execution_lease(owner_scope, run_id);
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish(std::string_view owner_scope, std::string_view expected_journal_head, ProgramTransitionPublication publication) override {
        co_await turns_->turn("transition", "compare_publish");
        co_return co_await inner_->compare_publish(owner_scope, expected_journal_head, std::move(publication));
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish_execution(std::string_view owner_scope, std::string_view expected_journal_head, ProgramTransitionPublication publication, std::optional<ProgramExecutionLease> expected_lease, std::optional<ProgramExecutionLease> next_lease) override {
        co_await turns_->turn("transition", "compare_publish_execution");
        co_return co_await inner_->compare_publish_execution(owner_scope, expected_journal_head, std::move(publication), std::move(expected_lease), std::move(next_lease));
    }
    asio::awaitable<ProgramTransitionPublishResult> compare_publish_graph_safe_point(std::string_view owner_scope, std::string_view expected_journal_head, ProgramTransitionPublication publication, const ProgramGraphSafePointEvidence& evidence, const GraphMigrationCapsule& capsule, const ProgramExecutionLease& execution_lease) override {
        co_await turns_->turn("transition", "compare_publish_graph_safe_point");
        co_return co_await inner_->compare_publish_graph_safe_point(owner_scope, expected_journal_head, std::move(publication), evidence, capsule, execution_lease);
    }

private:
    std::shared_ptr<ProgramTransitionStore> inner_;
    std::shared_ptr<HostTurns>              turns_;
};

class HostPumpedProgramStore final : public ProgramStore {
public:
    HostPumpedProgramStore(std::shared_ptr<ProgramStore> inner, std::shared_ptr<HostTurns> turns)
        : inner_(std::move(inner)), turns_(std::move(turns)) {}

    asio::awaitable<void> publish_admitted(const ProgramBundle& bundle, const ProgramVersion& version) override {
        co_await turns_->turn("program", "publish_admitted");
        co_await inner_->publish_admitted(bundle, version);
    }
    asio::awaitable<std::optional<ProgramBundle>> get_bundle(std::string_view id) const override {
        co_await turns_->turn("program", "get_bundle");
        co_return co_await inner_->get_bundle(id);
    }
    asio::awaitable<std::optional<ProgramVersion>> get_version(std::string_view id) const override {
        co_await turns_->turn("program", "get_version");
        co_return co_await inner_->get_version(id);
    }
    asio::awaitable<std::optional<ProgramBundle>> get_bundle(std::string_view owner_scope,
                                                             std::string_view id) const override {
        co_await turns_->turn("program", "get_bundle");
        co_return co_await inner_->get_bundle(owner_scope, id);
    }
    asio::awaitable<std::optional<ProgramVersion>> get_version(std::string_view owner_scope,
                                                               std::string_view id) const override {
        co_await turns_->turn("program", "get_version");
        co_return co_await inner_->get_version(owner_scope, id);
    }
    asio::awaitable<std::optional<ProgramActivation>> get_activation(std::string_view owner_scope) const override {
        co_await turns_->turn("program", "get_activation");
        co_return co_await inner_->get_activation(owner_scope);
    }
    asio::awaitable<std::optional<ProgramActivationBinding>> get_active_binding(
        std::string_view owner_scope) const override {
        co_await turns_->turn("program", "get_active_binding");
        co_return co_await inner_->get_active_binding(owner_scope);
    }
    asio::awaitable<ProgramActivationResult> compare_activate(std::string_view owner_scope,
                                                              std::uint64_t expected_generation,
                                                              std::string_view version_id,
                                                              std::string_view policy_snapshot_hash) override {
        co_await turns_->turn("program", "compare_activate");
        co_return co_await inner_->compare_activate(owner_scope, expected_generation, version_id,
                                                    policy_snapshot_hash);
    }
    asio::awaitable<std::vector<ProgramVersion>> list_versions(std::string_view owner_scope) const override {
        co_await turns_->turn("program", "list_versions");
        co_return co_await inner_->list_versions(owner_scope);
    }
    asio::awaitable<ProgramRetentionReport> collect_garbage(std::string_view owner_scope,
                                                            const std::vector<std::string>& pinned) override {
        co_await turns_->turn("program", "collect_garbage");
        co_return co_await inner_->collect_garbage(owner_scope, pinned);
    }

private:
    std::shared_ptr<ProgramStore> inner_;
    std::shared_ptr<HostTurns>    turns_;
};

class HostPumpedGrantStore final : public ProgramCoreToolGrantStore {
public:
    HostPumpedGrantStore(std::shared_ptr<ProgramCoreToolGrantStore> inner, std::shared_ptr<HostTurns> turns)
        : inner_(std::move(inner)), turns_(std::move(turns)) {}

    asio::awaitable<ProgramCoreToolGrantAdmission> admit(const ProgramCoreToolGrantRecord& record) override {
        co_await turns_->turn("grant", "admit");
        co_return co_await inner_->admit(record);
    }
    asio::awaitable<std::optional<ProgramCoreToolGrantRecord>> load(
        const ProgramCoreToolGrantContext& context) const override {
        co_await turns_->turn("grant", "load");
        co_return co_await inner_->load(context);
    }
    asio::awaitable<bool> revoke(const ProgramCoreToolGrantContext& context) override {
        co_await turns_->turn("grant", "revoke");
        co_return co_await inner_->revoke(context);
    }

private:
    std::shared_ptr<ProgramCoreToolGrantStore> inner_;
    std::shared_ptr<HostTurns>                 turns_;
};

class HostPumpedFragmentStore final : public TaskGraphFragmentStore {
public:
    HostPumpedFragmentStore(std::shared_ptr<TaskGraphFragmentStore> inner, std::shared_ptr<HostTurns> turns)
        : inner_(std::move(inner)), turns_(std::move(turns)) {}

    asio::awaitable<TaskGraphPublishResult> publish(const TaskGraphFragmentRecord& record) override {
        co_await turns_->turn("fragment", "publish");
        co_return co_await inner_->publish(record);
    }
    asio::awaitable<std::optional<TaskGraphFragmentRecord>> load(std::string_view fragment_id) const override {
        co_await turns_->turn("fragment", "load");
        co_return co_await inner_->load(fragment_id);
    }
    asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published_lineage(
        std::string_view owner_scope, std::string_view parent_run_id,
        std::string_view expansion_operation_id) const override {
        co_await turns_->turn("fragment", "find_published_lineage");
        co_return co_await inner_->find_published_lineage(owner_scope, parent_run_id, expansion_operation_id);
    }
    asio::awaitable<std::optional<TaskGraphFragmentRecord>> find_published(
        std::string_view owner_scope, std::string_view parent_run_id, std::string_view expansion_operation_id,
        std::string_view proposal_hash) const override {
        co_await turns_->turn("fragment", "find_published");
        co_return co_await inner_->find_published(owner_scope, parent_run_id, expansion_operation_id,
                                                  proposal_hash);
    }
    asio::awaitable<TaskGraphPublishResult> compare_update(std::string_view fragment_id,
                                                           std::uint64_t expected_revision,
                                                           const TaskGraphFragmentRecord& record) override {
        co_await turns_->turn("fragment", "compare_update");
        co_return co_await inner_->compare_update(fragment_id, expected_revision, record);
    }

private:
    std::shared_ptr<TaskGraphFragmentStore> inner_;
    std::shared_ptr<HostTurns>              turns_;
};

class HostPumpedModuleStore final : public ModuleStore {
public:
    HostPumpedModuleStore(std::shared_ptr<ModuleStore> inner, std::shared_ptr<HostTurns> turns)
        : inner_(std::move(inner)), turns_(std::move(turns)) {}

    asio::awaitable<void> publish(const ProgramModule& module) override {
        co_await turns_->turn("module", "publish");
        co_await inner_->publish(module);
    }
    asio::awaitable<std::optional<ProgramModule>> get(std::string_view content_identity) const override {
        co_await turns_->turn("module", "get");
        co_return co_await inner_->get(content_identity);
    }
    asio::awaitable<std::optional<ProgramModule>> get(std::string_view owner_scope,
                                                      std::string_view content_identity) const override {
        co_await turns_->turn("module", "get");
        co_return co_await inner_->get(owner_scope, content_identity);
    }
    asio::awaitable<void> set_lifecycle(std::string_view content_identity, ModuleLifecycle lifecycle) override {
        co_await turns_->turn("module", "set_lifecycle");
        co_await inner_->set_lifecycle(content_identity, std::move(lifecycle));
    }

private:
    std::shared_ptr<ModuleStore> inner_;
    std::shared_ptr<HostTurns>   turns_;
};

}  // namespace neograph::program::test_support
