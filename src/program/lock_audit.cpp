#include "lock_audit.h"

#if NEOGRAPH_PROGRAM_LOCK_AUDIT_ENABLED

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <unordered_map>

#if defined(__linux__) && __has_include(<execinfo.h>)
#include <execinfo.h>
#define NEOGRAPH_LOCK_AUDIT_BACKTRACE 1
#endif

namespace neograph::program::detail {

unsigned& pure_locks_held() noexcept {
    thread_local unsigned held = 0;
    return held;
}

void assert_no_pure_lock_held(const char* call) noexcept {
    if (pure_locks_held() == 0) return;
    std::fprintf(stderr,
                 "NeoGraph Program lock audit: %s was called while this thread holds %u mutex(es) that guard "
                 "memory only. A critical section that reaches a store uses an OperationLock.\n",
                 call, pure_locks_held());
#ifdef NEOGRAPH_LOCK_AUDIT_BACKTRACE
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, count, 2);
#endif
    std::abort();
}

namespace {

class AuditedTransitionStore final : public ProgramTransitionStore {
public:
    explicit AuditedTransitionStore(std::shared_ptr<ProgramTransitionStore> inner) : inner_(std::move(inner)) {}

    std::string process_coordination_key() const override {
        // Two wrappers over one store are one backend to the runtime, as the store itself is.
        auto key = inner_->process_coordination_key();
        if (!key.empty()) return key;
        std::ostringstream address;
        address << "audit-store:" << static_cast<const void*>(inner_.get());
        return address.str();
    }
    std::optional<ProgramRunRecord> load(std::string_view owner, std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load");
        return inner_->load(owner, run);
    }
    std::optional<ProgramJournalRecord> latest(std::string_view owner, std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::latest");
        return inner_->latest(owner, run);
    }
    std::optional<ProgramRunPublicationHead> load_run_publication_head(std::string_view owner,
                                                                       std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_run_publication_head");
        return inner_->load_run_publication_head(owner, run);
    }
    std::vector<ProgramEvent> load_events(std::string_view owner, std::string_view run,
                                          std::uint64_t after) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_events");
        return inner_->load_events(owner, run, after);
    }
    std::vector<ProgramEffectOutboxEntry> load_effects(std::string_view owner, std::string_view run,
                                                       std::uint64_t after) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_effects");
        return inner_->load_effects(owner, run, after);
    }
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner, std::string_view run, std::uint64_t after) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_javascript_commands");
        return inner_->load_javascript_commands(owner, run, after);
    }
    std::optional<ProgramCommandPublicationHead> load_command_publication_head(
        std::string_view owner, std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_command_publication_head");
        return inner_->load_command_publication_head(owner, run);
    }
    std::vector<ProgramContextPublication> load_context_publications(std::string_view owner, std::string_view run,
                                                                     std::uint64_t after) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_context_publications");
        return inner_->load_context_publications(owner, run, after);
    }
    std::vector<HookOutboxEntry> load_hook_outbox_entries(std::string_view owner,
                                                          std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_hook_outbox_entries");
        return inner_->load_hook_outbox_entries(owner, run);
    }
    ProgramEffectiveRuntimeState load_effective_runtime_state(std::string_view owner,
                                                              std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_effective_runtime_state");
        return inner_->load_effective_runtime_state(owner, run);
    }
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses(std::string_view owner,
                                                                  std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_child_syntheses");
        return inner_->load_child_syntheses(owner, run);
    }
    std::optional<MigrationPlan> load_migration_plan(std::string_view owner, std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_migration_plan");
        return inner_->load_migration_plan(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner, std::string_view lineage) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_lineage");
        return inner_->load_lineage(owner, lineage);
    }
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner, std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_run_lineage");
        return inner_->load_run_lineage(owner, run);
    }
    std::optional<ProgramRunLineage> load_lineage_head(std::string_view owner, std::string_view lineage,
                                                       std::string_view head) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_lineage_head");
        return inner_->load_lineage_head(owner, lineage, head);
    }
    std::optional<ProgramRunGeneration> load_generation(std::string_view owner, std::string_view lineage,
                                                        std::uint64_t generation) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_generation");
        return inner_->load_generation(owner, lineage, generation);
    }
    std::optional<ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner, std::string_view lineage, std::uint64_t generation) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_generation_initial_publication");
        return inner_->load_generation_initial_publication(owner, lineage, generation);
    }
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(std::string_view owner, std::string_view run,
                                                                      std::string_view head) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_graph_migration_capsule");
        return inner_->load_graph_migration_capsule(owner, run, head);
    }
    std::optional<ProgramExecutionLease> load_execution_lease(std::string_view owner,
                                                              std::string_view run) const override {
        assert_no_pure_lock_held("ProgramTransitionStore::load_execution_lease");
        return inner_->load_execution_lease(owner, run);
    }
    ProgramTransitionPublishResult compare_publish(std::string_view owner, std::string_view expected,
                                                   ProgramTransitionPublication publication) override {
        assert_no_pure_lock_held("ProgramTransitionStore::compare_publish");
        return inner_->compare_publish(owner, expected, std::move(publication));
    }
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner, std::string_view expected, ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override {
        assert_no_pure_lock_held("ProgramTransitionStore::compare_publish_execution");
        return inner_->compare_publish_execution(owner, expected, std::move(publication), std::move(expected_lease),
                                                 std::move(next_lease));
    }
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner, std::string_view expected, ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence, const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& lease) override {
        assert_no_pure_lock_held("ProgramTransitionStore::compare_publish_graph_safe_point");
        return inner_->compare_publish_graph_safe_point(owner, expected, std::move(publication), evidence, capsule,
                                                        lease);
    }

private:
    std::shared_ptr<ProgramTransitionStore> inner_;
};

}  // namespace

std::shared_ptr<ProgramTransitionStore> audit_transition_store(std::shared_ptr<ProgramTransitionStore> inner) {
    if (!inner) return inner;
    // One wrapper per store, so that handles of two runtimes over one store still name the same store.
    static std::mutex registry_mutex;
    static std::unordered_map<const ProgramTransitionStore*, std::weak_ptr<AuditedTransitionStore>> registry;
    std::lock_guard lock(registry_mutex);
    std::erase_if(registry, [](const auto& entry) { return entry.second.expired(); });
    auto& slot = registry[inner.get()];
    if (auto live = slot.lock()) return live;
    auto wrapper = std::make_shared<AuditedTransitionStore>(std::move(inner));
    slot         = wrapper;
    return wrapper;
}

}  // namespace neograph::program::detail

#endif
