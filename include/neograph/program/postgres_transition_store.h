/**
 * @file program/postgres_transition_store.h
 * @brief PostgreSQL-backed atomic Program transition publication.
 */
#pragma once

#include <neograph/program/transition_store.h>

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
    : public ProgramTransitionStore {
public:
    explicit PostgreSQLProgramTransitionStore(std::string connection_string);
    PostgreSQLProgramTransitionStore(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore& operator=(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore(const PostgreSQLProgramTransitionStore&) = delete;
    PostgreSQLProgramTransitionStore& operator=(const PostgreSQLProgramTransitionStore&) = delete;
    ~PostgreSQLProgramTransitionStore() override;

    std::string process_coordination_key() const override;

    std::optional<ProgramRunRecord> load(std::string_view owner_scope,
                                         std::string_view run_id) const override;
    std::optional<ProgramJournalRecord> latest(std::string_view owner_scope,
                                                std::string_view run_id) const override;
    std::vector<ProgramEvent> load_events(std::string_view owner_scope,
                                           std::string_view run_id,
                                           std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramEffectOutboxEntry> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const override;
    std::vector<HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<MigrationPlan> load_migration_plan(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage(
        std::string_view owner_scope, std::string_view lineage_id) const override;
    std::optional<ProgramRunLineage> load_run_lineage(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::string_view head_id) const override;
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const override;
    std::optional<ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const override;
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view owner_scope,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const override;
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner_scope, std::string_view run_id) const override;

    ProgramTransitionPublishResult compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication) override;
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override;
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
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
