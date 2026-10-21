/**
 * @file program/sqlite_transition_store.h
 * @brief SQLite-backed atomic Program transition publication.
 */
#pragma once

#include <neograph/program/transition_store.h>

#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Durable owner-scoped ProgramTransitionStore.
 *
 * Each publication is stored as one canonical immutable blob and committed in
 * one SQLite transaction.  The blob boundary deliberately mirrors the
 * backend-neutral publication contract: a restart can expose either the
 * previous complete publication or the next complete publication, never a
 * partially written run/journal/event/effect set.
 */
class NEOGRAPH_PROGRAM_API SQLiteProgramTransitionStore final
    : public SyncProgramTransitionStore {
public:
    explicit SQLiteProgramTransitionStore(std::string database_path);
    SQLiteProgramTransitionStore(SQLiteProgramTransitionStore&&) noexcept;
    SQLiteProgramTransitionStore& operator=(SQLiteProgramTransitionStore&&) noexcept;
    SQLiteProgramTransitionStore(const SQLiteProgramTransitionStore&) = delete;
    SQLiteProgramTransitionStore& operator=(const SQLiteProgramTransitionStore&) = delete;
    ~SQLiteProgramTransitionStore() override;

    std::string process_coordination_key() const override;

    std::optional<ProgramRunPublicationHead> load_run_publication_head_sync(
        std::string_view owner_scope, std::string_view run_id) const override;

    std::optional<ProgramCommandPublicationHead> load_command_publication_head_sync(
        std::string_view owner_scope, std::string_view run_id) const override;

    std::optional<ProgramRunRecord> load_sync(std::string_view owner_scope,
                                         std::string_view run_id) const override;
    std::optional<ProgramJournalRecord> latest_sync(std::string_view owner_scope,
                                               std::string_view run_id) const override;
    std::vector<ProgramEvent> load_events_sync(std::string_view owner_scope,
                                           std::string_view run_id,
                                           std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramEffectOutboxEntry>
    load_effects_sync(std::string_view owner_scope,
                 std::string_view run_id,
                 std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramJavaScriptCommandJournalEntry>
    load_javascript_commands_sync(std::string_view owner_scope,
                             std::string_view run_id,
                             std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner_scope,
        std::string_view run_id,
         std::uint64_t    after_sequence = 0) const override;
    std::vector<HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<MigrationPlan> load_migration_plan_sync(std::string_view owner_scope,
                                                     std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_sync(std::string_view owner_scope,
                                                    std::string_view lineage_id) const override;
    std::optional<ProgramRunLineage> load_run_lineage_sync(std::string_view owner_scope,
                                                       std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_head_sync(std::string_view owner_scope,
                                                        std::string_view lineage_id,
                                                        std::string_view head_id) const override;
    std::optional<ProgramRunGeneration> load_generation_sync(std::string_view owner_scope,
                                                          std::string_view lineage_id,
                                                          std::uint64_t generation) const override;
    std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const override;
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view owner_scope,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override;
    std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view owner_scope,
        std::string_view run_id) const override;
    ProgramTransitionPublishResult
    compare_publish_sync(std::string_view owner_scope,
                    std::string_view expected_journal_head,
                    ProgramTransitionPublication publication) override;
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override;
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override;

private:
    ProgramTransitionPublishResult compare_publish_impl(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence* evidence,
        const GraphMigrationCapsule* capsule,
        const ProgramExecutionLease* expected_lease,
        const ProgramExecutionLease* next_lease);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
