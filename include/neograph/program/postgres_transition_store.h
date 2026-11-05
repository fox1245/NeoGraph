/**
 * @file program/postgres_transition_store.h
 * @brief PostgreSQL-backed atomic Program transition publication.
 */
#pragma once

#include <neograph/program/transition_store.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

namespace detail {

/**
 * Points in a batched publication at which a test runs code, and faults it makes. For tests only: no product code
 * sets them, and none of them waits. An unset member does nothing.
 */
struct PublicationTestHooks {
    /// After the read groups were read and before the validator runs.
    std::function<void()> after_reads;
    /// After the publication was validated and before its write statements are built.
    std::function<void()> before_write;
    /// After the write statements are built and before they and the COMMIT are sent.
    std::function<void()> before_commit;
    /// The write statements of the next publications contain one statement the server refuses.
    bool fail_write = false;
    /// The next pipeline leaves the connection in pipeline mode instead of exiting it.
    bool fail_pipeline_leave = false;
    /// The publication does not take the owner-scope lock: the writer a peer process without it would be.
    bool skip_owner_lock = false;
};

}  // namespace detail

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

        /**
         * Maximum number of runs whose last published head this instance remembers for
         * ProgramTransitionStore::own_head_hint; 0 disables the hint. Least-recently-used entries are
         * evicted when the table is full.
         */
        std::size_t own_head_hint_capacity = 256;

        /**
         * Maximum number of run generations kept in memory. A stored generation never changes, and a reader
         * compares what it takes from the cache with the lineage head that names it; 0 disables the cache.
         */
        std::size_t generation_cache_capacity = 256;

        /**
         * A publication reads the rows it will lock (lineage heads, run heads with their execution lease, relation
         * heads and rows) in at most four ordered statements before it validates, and the validator finds them
         * already read. A row that the publication's typed parts do not name is read, locked, when the validator asks,
         * as before; late_lock_count() counts those reads. False sends one statement per question, as earlier
         * versions did; for intact stored rows the verdicts and the stored rows are the same either way. On either path,
         * a transaction that ends with the connection still in pipeline mode, or not idle outside a transaction,
         * resets the connection.
         */
        bool batched_publication = true;

        /**
         * With batched_publication, the statements of a publication are sent in two pipelines (libpq pipeline mode,
         * libpq 14 or later): BEGIN, the owner lock and the read groups; then the writes and COMMIT, each pipeline
         * with one synchronization point and one round trip. A statement that fails rolls the transaction back and
         * throws, and so does a COMMIT that the server answered as a rollback. The COMMIT goes out with the writes,
         * before any write is answered: when the connection is lost after that batch was sent, the publication throws
         * although the server may have committed it, as a COMMIT whose answer is lost may have committed; with one
         * statement at a time that window is only the COMMIT's own round trip. False, or a libpq without pipeline
         * mode, sends the same statements one after the other, with the same results.
         */
        bool use_pipeline = true;

        /**
         * With batched_publication, how many times a publication is decided again after another writer inserted a row
         * it inserts because it found it absent (the owner lock makes that unreachable; a writer without it can reach
         * it). The transaction is rolled back and the publication runs again against what is stored; when it loses
         * the race this many times it returns Conflict, which the caller answers by reading the stored head again. 0
         * returns Conflict at the first loss.
         */
        std::size_t lost_race_replans = 1;
    };

    explicit PostgreSQLProgramTransitionStore(std::string connection_string);
    PostgreSQLProgramTransitionStore(std::string connection_string, Options options);
    PostgreSQLProgramTransitionStore(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore& operator=(PostgreSQLProgramTransitionStore&&) noexcept;
    PostgreSQLProgramTransitionStore(const PostgreSQLProgramTransitionStore&) = delete;
    PostgreSQLProgramTransitionStore& operator=(const PostgreSQLProgramTransitionStore&) = delete;
    ~PostgreSQLProgramTransitionStore() override;

    /// For tests only: sets the hooks a publication of this store runs. Pass null to remove them.
    void set_publication_test_hooks(std::shared_ptr<detail::PublicationTestHooks> hooks);

    /// The number of statements this store has sent on its connection, one per statement.
    std::uint64_t statements_sent() const noexcept;
    /// The number of reads that took a row lock the publication's read groups had not taken: the lock order of
    /// batched_publication was not the whole order of that publication.
    std::uint64_t late_lock_count() const noexcept;
    /// The server process id of the store's open connection (empty when it has none), for a test that looks it up in
    /// pg_locks or pg_stat_activity.
    std::vector<int> connection_backend_pids() const;

    std::string process_coordination_key() const override;
    std::optional<ProgramOwnHeadHint> own_head_hint(std::string_view owner_scope,
                                                    std::string_view run_id) const override;
    void forget_own_head(std::string_view owner_scope, std::string_view run_id) const noexcept override;

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
    void verify_stored_history_sync(std::string_view owner_scope,
                                    std::string_view run_id) const override;
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

    std::optional<ProgramChildRelationHead> load_child_relation_head_sync(
        std::string_view owner_scope, std::string_view parent_run_id) const override;
    std::optional<ProgramChildRelation> load_child_relation_sync(
        std::string_view owner_scope, std::string_view parent_run_id,
        std::string_view child_run_id) const override;
    std::vector<ProgramChildRelation> load_child_relations_sync(
        std::string_view owner_scope, std::string_view parent_run_id, bool open_only) const override;
    ProgramChildRelationOutcome insert_child_relation_sync(
        std::string_view owner_scope, std::string_view parent_run_id,
        std::uint64_t expected_parent_fence, ProgramChildRecord child) override;
    ProgramChildRelationOutcome update_child_relation_sync(
        std::string_view owner_scope, std::string_view parent_run_id,
        std::uint64_t expected_parent_fence, std::optional<std::uint64_t> expected_row_version,
        ProgramChildRecord next) override;
    ProgramTransitionPublishResult compare_publish_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication) override;
    ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) override;
    ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) override;
    ProgramTransitionPublishResult compare_publish_takeover_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence) override;

protected:
    /** libpq blocks for long: an awaited operation runs on a thread of the bounded pool (#556). */
    bool offloads() const noexcept override { return true; }

private:
    ProgramTransitionPublishResult compare_publish_impl(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence* evidence,
        const GraphMigrationCapsule* capsule,
        const ProgramExecutionLease* expected_lease,
        const ProgramExecutionLease* next_lease);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
