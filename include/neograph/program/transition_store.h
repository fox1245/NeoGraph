/**
 * @file program/transition_store.h
 * @brief Atomic owner-scoped Program transition publication.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/command_journal.h>
#include <neograph/program/event.h>
#include <neograph/program/lineage.h>
#include <neograph/program/migration.h>
#include <neograph/program/run_record.h>
#include <neograph/hook_outbox.h>
#include <neograph/runtime_context.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Exact context evidence selected for one Program transition. */
struct NEOGRAPH_PROGRAM_API ProgramContextPublication {
    ContextEpoch                 epoch;
    std::vector<ContextArtifact> artifacts;
    ContextAssemblyReceipt       assembly_receipt;
};

NEOGRAPH_PROGRAM_API void validate_program_context_publication(
    const ProgramContextPublication& context, const ProgramRunRecord& run);
NEOGRAPH_PROGRAM_API bool is_valid_program_context_history_append(
    const std::vector<ProgramContextPublication>& old_context,
    const std::optional<ProgramContextPublication>& next_context);

/** Validates a durable hook outbox head before it is attached to a Program run. */
NEOGRAPH_PROGRAM_API void validate_program_hook_outbox_entry(
    const HookOutboxEntry& entry, const ProgramRunRecord& run);
/**
 * Validates an append-only hook-head update. Entries not present in `next` retain
 * their prior logical invocation head; a new entry may only advance its own head.
 */
NEOGRAPH_PROGRAM_API bool is_valid_program_hook_history_append(
    const std::vector<HookOutboxEntry>& old_entries,
    const std::vector<HookOutboxEntry>& next_entries,
    const ProgramRunRecord& run);

/** Effective runtime state for one generation, preserving source hook provenance. */
struct NEOGRAPH_PROGRAM_API ProgramEffectiveRuntimeState {
    std::vector<ProgramContextPublication> context_publications;
    std::vector<HookOutboxEntry> hook_outbox_entries;
    std::vector<HookOutboxEntry> inherited_hook_outbox_entries;
    std::optional<ProgramRuntimeStateTransferReceipt> transfer_receipt;
};

/** Validates exact context and unresolved-hook transfer into one successor. */
NEOGRAPH_PROGRAM_API bool is_valid_program_runtime_state_transfer(
    const std::optional<ProgramRuntimeStateTransferReceipt>& receipt,
    const ProgramRunGeneration& source_generation,
    const ProgramRunLineage& source_lineage,
    const ProgramRunRecord& source_run,
    const std::vector<ProgramContextPublication>& source_contexts,
    const std::vector<HookOutboxEntry>& source_hooks,
    const ProgramRunGeneration& target_generation,
    const ProgramRunRecord& target_run,
    const std::optional<ProgramContextPublication>& target_context) noexcept;

/** Validates a source snapshot cloned into an independent fork lineage. */
NEOGRAPH_PROGRAM_API bool is_valid_program_runtime_context_clone(
    const std::vector<ProgramContextPublication>& source_contexts,
    const ProgramRunRecord& target_run,
    const std::optional<ProgramContextPublication>& target_context) noexcept;

class NEOGRAPH_PROGRAM_API ProgramEffectOutboxEntry {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    ProgramEffectOutboxEntry(std::uint64_t sequence, ProgramPendingEffect effect);
    static ProgramEffectOutboxEntry parse(std::string_view stored_bytes);

    std::uint64_t sequence() const noexcept;
    const ProgramPendingEffect& effect() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramEffectOutboxEntry(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct NEOGRAPH_PROGRAM_API ProgramTransitionPublication {
    ProgramRunRecord                      run_record;
    ProgramJournalRecord                  journal_record;
    std::vector<ProgramEvent>              events;
    std::vector<ProgramEffectOutboxEntry>  effects;
    /// Present only on a migration/fork publication; subsequent transitions
    /// inherit it from the durable run publication.
    std::optional<MigrationPlan>            migration_plan;
    std::vector<ProgramJavaScriptCommandJournalEntry> commands;
    /// Present on lineage-aware publications. A generation accompanies only
    /// the first head or an admitted successor; ordinary updates omit it.
    std::optional<ProgramRunGeneration>      run_generation;
    std::optional<ProgramRunLineage>         run_lineage;
    /// On a fork's first publication, atomically debits the still-active source
    /// lineage while the target starts in an independent generation-one lineage.
    std::optional<ProgramRunLineage>         fork_source_lineage;
    /// Optional append-only provider-context evidence for this transition.
    std::optional<ProgramContextPublication>  context_publication;
    /// Immutable hook outbox heads appended atomically with this transition.
    std::vector<HookOutboxEntry>               hook_outbox_entries;
    static ProgramTransitionPublication parse(std::string_view stored_bytes);
    std::string serialize_canonical() const;
};

enum class ProgramTransitionPublishResult : std::uint8_t {
    Published,
    AlreadyPresent,
    Conflict,
};

/// One-shot in-memory publication faults used to exercise the strong
/// exception guarantee at each staged publication boundary.
enum class ProgramTransitionFaultPoint : std::uint8_t {
    AfterRunSnapshot,
    AfterJournalSnapshot,
    AfterEventSnapshot,
    AfterEffectSnapshot,
    AfterContextSnapshot,
    AfterHookSnapshot,
    AfterLineageSnapshot,
    BeforeCommit,
};

class NEOGRAPH_PROGRAM_API ProgramTransitionStore {
public:
    virtual ~ProgramTransitionStore() = default;

    /** Process-local namespace used to coordinate wrappers over one backend. */
    virtual std::string process_coordination_key() const { return {}; }

    /** Wrong-owner lookups are indistinguishable from absence. */
    virtual std::optional<ProgramRunRecord> load(std::string_view owner_scope,
                                                  std::string_view run_id) const = 0;
    virtual std::optional<ProgramJournalRecord> latest(std::string_view owner_scope,
                                                        std::string_view run_id) const = 0;
    virtual std::vector<ProgramEvent> load_events(std::string_view owner_scope,
                                                   std::string_view run_id,
                                                   std::uint64_t after_sequence = 0) const = 0;
    virtual std::vector<ProgramEffectOutboxEntry>
    load_effects(std::string_view owner_scope,
                 std::string_view run_id,
                 std::uint64_t after_sequence = 0) const = 0;
    /**
     * Durable yielded-command journal, ordered by append sequence.
     *
     * The base implementation fails closed. Stores that participate in
     * JavaScript control recovery must override this operation rather than
     * making an unreadable journal look empty.
     */
    virtual std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t    after_sequence = 0) const;
    /** Durable provider-context evidence, ordered by ContextEpoch sequence. */
    virtual std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t    after_sequence = 0) const;
    /** Current durable heads, including pending and reconciliation-required hooks. */
    virtual std::vector<HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner_scope, std::string_view run_id) const;
    /** Target-local state plus source-origin hook heads authorized by its generation. */
    virtual ProgramEffectiveRuntimeState load_effective_runtime_state(
        std::string_view owner_scope, std::string_view run_id) const;
    /** Durable migration proof published with a fork, if this run is a fork. */
    virtual std::optional<MigrationPlan>
    load_migration_plan(std::string_view /*owner_scope*/, std::string_view /*run_id*/) const {
        return std::nullopt;
    }
    /** Current immutable lineage head, when the run uses generation accounting. */
    virtual std::optional<ProgramRunLineage>
    load_lineage(std::string_view /*owner_scope*/, std::string_view /*lineage_id*/) const {
        return std::nullopt;
    }
    /** Current lineage head associated with one run generation. */
    virtual std::optional<ProgramRunLineage>
    load_run_lineage(std::string_view owner_scope, std::string_view run_id) const;
    /** One immutable historical lineage head addressed by its content identity. */
    virtual std::optional<ProgramRunLineage>
    load_lineage_head(std::string_view /*owner_scope*/,
                      std::string_view /*lineage_id*/,
                      std::string_view /*head_id*/) const {
        return std::nullopt;
    }
    /** One immutable topology generation retained by ordinal. */
    virtual std::optional<ProgramRunGeneration>
    load_generation(std::string_view /*owner_scope*/,
                    std::string_view /*lineage_id*/,
                    std::uint64_t /*generation*/) const {
        return std::nullopt;
    }
    /** Exact immutable publication that created one topology generation. */
    virtual std::optional<ProgramTransitionPublication>
    load_generation_initial_publication(std::string_view /*owner_scope*/,
                                        std::string_view /*lineage_id*/,
                                        std::uint64_t /*generation*/) const;
    /** Immutable source hold captured atomically with one graph safe point. */
    virtual std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view /*owner_scope*/,
        std::string_view /*source_run_id*/,
        std::string_view /*source_lineage_head_id*/) const {
        return std::nullopt;
    }
    /** Current backend-global root Core dispatch owner, when one is active. */
    virtual std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view /*owner_scope*/, std::string_view /*run_id*/) const {
        return std::nullopt;
    }

    /**
     * Atomically publishes the run snapshot, journal head, events, effect outbox,
     * and JavaScript command appends. A throw has the strong exception guarantee.
     * An exact retry is AlreadyPresent.
     */
    virtual ProgramTransitionPublishResult
    compare_publish(std::string_view owner_scope,
                    std::string_view expected_journal_head,
                    ProgramTransitionPublication publication) = 0;
    /** Atomically publishes and acquires, releases, or replaces a dispatch lease. */
    virtual ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) {
        return ProgramTransitionPublishResult::Conflict;
    }
    /** Host-only peer for a running source checkpoint; evidence is never serialized. */
    virtual ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) {
        return ProgramTransitionPublishResult::Conflict;
    }
};

class NEOGRAPH_PROGRAM_API InMemoryProgramTransitionStore final
    : public ProgramTransitionStore {
public:
    InMemoryProgramTransitionStore();
    ~InMemoryProgramTransitionStore() override;
    InMemoryProgramTransitionStore(InMemoryProgramTransitionStore&&) noexcept;
    InMemoryProgramTransitionStore& operator=(InMemoryProgramTransitionStore&&) noexcept;
    InMemoryProgramTransitionStore(const InMemoryProgramTransitionStore&) = delete;
    InMemoryProgramTransitionStore& operator=(const InMemoryProgramTransitionStore&) = delete;

    std::optional<ProgramRunRecord> load(std::string_view owner_scope,
                                          std::string_view run_id) const override;
    std::optional<ProgramJournalRecord> latest(std::string_view owner_scope,
                                                std::string_view run_id) const override;
    std::vector<ProgramEvent> load_events(std::string_view owner_scope,
                                           std::string_view run_id,
                                           std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramEffectOutboxEntry>
    load_effects(std::string_view owner_scope,
                 std::string_view run_id,
                 std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramJavaScriptCommandJournalEntry>
    load_javascript_commands(std::string_view owner_scope,
                              std::string_view run_id,
                              std::uint64_t after_sequence = 0) const override;
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
                               std::uint64_t    after_sequence = 0) const override;
    std::vector<HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner_scope, std::string_view run_id) const override;
    std::optional<MigrationPlan> load_migration_plan(std::string_view owner_scope,
                                                     std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage(std::string_view owner_scope,
                                                   std::string_view lineage_id) const override;
    std::optional<ProgramRunLineage> load_run_lineage(std::string_view owner_scope,
                                                       std::string_view run_id) const override;
    std::optional<ProgramRunLineage> load_lineage_head(std::string_view owner_scope,
                                                        std::string_view lineage_id,
                                                        std::string_view head_id) const override;
    std::optional<ProgramRunGeneration> load_generation(std::string_view owner_scope,
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
        std::string_view owner_scope,
        std::string_view run_id) const override;
    ProgramTransitionPublishResult
    compare_publish(std::string_view owner_scope,
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

    /// Fail the next publication at a staged boundary; test-only hook.
    void fail_next_publication_for_testing(ProgramTransitionFaultPoint point);

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
