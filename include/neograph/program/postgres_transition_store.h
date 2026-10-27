/**
 * @file program/postgres_transition_store.h
 * @brief PostgreSQL-backed atomic Program transition publication.
 */
#pragma once

#include <neograph/program/transition_store.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/**
 * Durable owner-scoped ProgramTransitionStore backed by PostgreSQL.
 *
 * Every publication executes in one PostgreSQL transaction and locks the
 * affected run/lineage heads before validating their compare-and-swap state.
 * Exact retries are idempotent and a failed validation or SQL operation leaves
 * every prior head, log, generation, capsule, and lease unchanged.
 */
class NEOGRAPH_PROGRAM_API PostgreSQLProgramTransitionStore final
    : public SyncProgramTransitionStore {
public:
    /** Store-level configuration options. */
    struct Options {
        /**
         * Maximum number of runs whose last parsed ProgramRunRecord is kept in
         * memory while its stored bytes are unchanged. Least-recently-used
         * entries are evicted when the cache is full; 0 disables the cache.
         */
        std::size_t run_record_cache_capacity = 256;
    };

    explicit PostgreSQLProgramTransitionStore(std::string connection_string);
    PostgreSQLProgramTransitionStore(std::string connection_string, Options options);
    PostgreSQLProgramTransitionStore(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore& operator=(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore(const PostgreSQLProgramTransitionStore&) = delete;
    PostgreSQLProgramTransitionStore& operator=(const PostgreSQLProgramTransitionStore&) = delete;
    ~PostgreSQLProgramTransitionStore() override;

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
    std::vector<ProgramEffectOutboxEntry> load_effects_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<MigrationPlan> load_migration_plan_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_sync(
        std::string_view owner_scope, std::string_view lineage_id) const override;
    std::optional<ProgramRunLineage> load_run_lineage_sync(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::string_view head_id) const override;
    std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view owner_scope,
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
        std::string_view owner_scope, std::string_view run_id) const override;

    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view owner_scope,
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

protected:
    /** libpq blocks for long: an awaited operation runs on a thread of the bounded pool (#556). */
    bool offloads() const noexcept override { return true; }

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
