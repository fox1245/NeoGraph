/**
 * @file program/transition_store.h
 * @brief Atomic owner-scoped Program transition publication.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/store_call.h>
#include <neograph/hook_outbox.h>
#include <neograph/program/child_relation.h>
#include <neograph/program/child_synthesis.h>
#include <neograph/program/command_journal.h>
#include <neograph/program/event.h>
#include <neograph/program/lineage.h>
#include <neograph/program/migration.h>
#include <neograph/program/run_record.h>
#include <neograph/runtime_context.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/** Bind a child result from an admitted replacement generation to its stable relation. */
NEOGRAPH_PROGRAM_API bool does_program_child_generation_result_bind(
    const ProgramRunRecord&     parent,
    const ProgramChildRecord&   child,
    const ProgramRunGeneration& generation,
    const ProgramRunLineage&    lineage,
    const ProgramRunRecord&     result_run) noexcept;

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

/** Exact source custody for a recorded replay; this is not a fresh dispatch grant. */
struct ProgramRecordedReplaySource {
    std::string run_id;
    std::string run_record_id;
    std::string journal_head;
    std::string lineage_head_id;
};

/**
 * The relation rows a publication changes in its own transaction (#661). A part is checked against the parent's
 * stored head and relation head by the store that commits the publication.
 */
struct ProgramChildRelationSpawn {
    /// Logical run id of the parent the child attaches to.
    std::string parent_run_id;
    /// The fence the writer holds on the parent; the store compares it with the parent's stored fence.
    std::uint64_t parent_fence = 0;
    /// The row to insert for the run this publication creates: state Dispatched, or Publishing.
    ProgramChildRecord child;
};
struct ProgramChildRelationCompletion {
    std::string   parent_run_id;
    std::uint64_t parent_fence = 0;
    /// When present, the row must hold exactly this version; otherwise the update applies to the stored row.
    std::optional<std::uint64_t> expected_row_version;
    /// The row's next content: the terminal state, its result and the generation that produced it.
    ProgramChildRecord child;
};
struct ProgramChildRelationPart {
    /// The parent publication holds only while the parent's relation epoch is still this value (0 when the
    /// parent has no rows). Set by a transition that decided from the relation set.
    std::optional<std::uint64_t>                  expected_epoch;
    /// Present on a child's first publication: the parent relation row is inserted in the same transaction, or
    /// the whole publication is refused.
    std::optional<ProgramChildRelationSpawn>      spawn;
    /// Present on a child's terminal publication: the parent relation row follows in the same transaction when
    /// the update is admitted. A refused update does not refuse the child's own publication.
    std::optional<ProgramChildRelationCompletion> completion;
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
    std::vector<ProgramChildSynthesisRecord>   child_synthesis_records;
    /// First-publication-only CAS allocation of retained replay work authority.
    std::optional<ProgramRecordedReplaySource> recorded_replay_source;
    std::optional<ProgramRunLineage> recorded_replay_source_lineage;
    /// Child relation rows changed in the same transaction as this publication (#661).
    std::optional<ProgramChildRelationPart> child_relations;
    static ProgramTransitionPublication parse(std::string_view stored_bytes);
    std::string serialize_canonical() const;
};

NEOGRAPH_PROGRAM_API bool is_valid_program_recorded_replay_allocation(
    const ProgramTransitionPublication& publication,
    const ProgramRunLineage& previous_source_lineage,
    const ProgramRunRecord& source_run);

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
    AfterRelationSnapshot,
    BeforeCommit,
};

/** A run record and journal from one committed publication, without optional command history. */
struct ProgramRunPublicationHead {
    ProgramRunRecord run_record;
    ProgramJournalRecord journal_record;
};

/** One publication head and its newest command from the same committed snapshot. */
struct ProgramCommandPublicationHead {
    ProgramRunRecord run_record;
    ProgramJournalRecord journal_record;
    std::optional<ProgramJavaScriptCommandJournalEntry> latest_command;
};

/**
 * The durable store of Program transitions, as the runtime uses it (#556). Every operation is awaitable: a store whose
 * backend is synchronous completes it at once (SyncProgramTransitionStore), and a host's store may complete it on a
 * later turn of the host's event loop. The arguments of a call, string views included, must stay alive until the
 * awaitable completes: await it in the full expression that makes the call.
 */
class NEOGRAPH_PROGRAM_API ProgramTransitionStore {
public:
    virtual ~ProgramTransitionStore() = default;

    /** Process-local namespace used to coordinate wrappers over one backend. */
    virtual std::string process_coordination_key() const { return {}; }

    /** Wrong-owner lookups are indistinguishable from absence. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunRecord>> load(
        std::string_view owner_scope,
        std::string_view run_id) const = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramJournalRecord>> latest(
        std::string_view owner_scope,
        std::string_view run_id) const = 0;
    /**
     * Read one coherent record/journal pair without requiring JavaScript support.
     * Native stores capture one snapshot. The compatibility implementation fences
     * virtual reads with a second record read and retries at most three times when
     * that head changes. Absence or exhausted contention fails closed; read errors
     * propagate. Acquisition grants no publication authority and retries no effects.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunPublicationHead>> load_run_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const;
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramEvent>> load_events(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const = 0;
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramEffectOutboxEntry>> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const = 0;
    /**
     * Durable yielded-command journal, ordered by append sequence.
     *
     * The base implementation fails closed. Stores that participate in
     * JavaScript control recovery must override this operation rather than
     * making an unreadable journal look empty.
     */
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramJavaScriptCommandJournalEntry>> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    /**
     * Read one coherent run/journal head and the last command append.
     * Native stores use a single snapshot without materializing command history.
     * The compatibility implementation fences the existing virtual reads with a
     * second head read; a changed head returns absence so publication fails closed.
     * This read grants no write authority: publication must still CAS the head.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramCommandPublicationHead>> load_command_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const;
    /** Durable provider-context evidence, ordered by ContextEpoch sequence. */
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramContextPublication>> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    /** Current durable heads, including pending and reconciliation-required hooks. */
    [[nodiscard]] virtual asio::awaitable<std::vector<HookOutboxEntry>> load_hook_outbox_entries(
        std::string_view owner_scope,
        std::string_view run_id) const;
    /** Target-local state plus source-origin hook heads authorized by its generation. */
    [[nodiscard]] virtual asio::awaitable<ProgramEffectiveRuntimeState> load_effective_runtime_state(
        std::string_view owner_scope,
        std::string_view run_id) const;
    /** Current synthesis heads. Unsupported stores fail closed, never report an empty journal. */
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramChildSynthesisRecord>> load_child_syntheses(
        std::string_view owner_scope,
        std::string_view run_id) const;
    /** Durable migration proof published with a fork, if this run is a fork. */
    [[nodiscard]] virtual asio::awaitable<std::optional<MigrationPlan>> load_migration_plan(
        std::string_view /*owner_scope*/,
        std::string_view /*run_id*/) const {
        co_return std::nullopt;
    }
    /** Current immutable lineage head, when the run uses generation accounting. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunLineage>> load_lineage(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/) const {
        co_return std::nullopt;
    }
    /** Current lineage head associated with one run generation. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunLineage>> load_run_lineage(
        std::string_view owner_scope,
        std::string_view run_id) const;
    /** One immutable historical lineage head addressed by its content identity. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunLineage>> load_lineage_head(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/,
        std::string_view /*head_id*/) const {
        co_return std::nullopt;
    }
    /** One immutable topology generation retained by ordinal. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramRunGeneration>> load_generation(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/,
        std::uint64_t /*generation*/) const {
        co_return std::nullopt;
    }
    /** Exact immutable publication that created one topology generation. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramTransitionPublication>> load_generation_initial_publication(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/,
        std::uint64_t /*generation*/) const;
    /** Immutable source hold captured atomically with one graph safe point. */
    [[nodiscard]] virtual asio::awaitable<std::optional<GraphMigrationCapsule>> load_graph_migration_capsule(
        std::string_view /*owner_scope*/,
        std::string_view /*source_run_id*/,
        std::string_view /*source_lineage_head_id*/) const {
        co_return std::nullopt;
    }
    /** Current backend-global root Core dispatch owner, when one is active. */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramExecutionLease>> load_execution_lease(
        std::string_view /*owner_scope*/,
        std::string_view /*run_id*/) const {
        co_return std::nullopt;
    }

    /**
     * The parent's relation head, or nothing when the parent has no rows (epoch 0). `parent_run_id` is the parent's
     * logical run id: a replacement generation of the parent reads the same rows.
     */
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramChildRelationHead>> load_child_relation_head(
        std::string_view owner_scope,
        std::string_view parent_run_id) const = 0;
    [[nodiscard]] virtual asio::awaitable<std::optional<ProgramChildRelation>> load_child_relation(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view child_run_id) const = 0;
    /** The parent's rows in attach order; with `open_only`, only the rows whose child is Publishing or Dispatched. */
    [[nodiscard]] virtual asio::awaitable<std::vector<ProgramChildRelation>> load_child_relations(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        bool             open_only = false) const = 0;
    /**
     * Inserts one relation row in a transaction that reads the parent's head. Applied when the parent is Running
     * with `expected_parent_fence`, the relation head is open, the row is new, and the child's permanent
     * reservation fits the parent's remaining budget on top of the head's aggregate. The row, the head's epoch,
     * counts and aggregate change together. An exact repeat of a stored row is AlreadyPresent; every other
     * refusal carries its reason and changes nothing.
     */
    [[nodiscard]] virtual asio::awaitable<ProgramChildRelationOutcome> insert_child_relation(
        std::string_view   owner_scope,
        std::string_view   parent_run_id,
        std::uint64_t      expected_parent_fence,
        ProgramChildRecord child) = 0;
    /**
     * Moves one relation row to its next state in a transaction that reads the parent's head. The writer's fence
     * is compared with the parent's stored fence; the row's identity, link, invocation and granted budget cannot
     * change; the state follows the transition rule; a terminal result must bind the child's active generation;
     * Publishing to Dispatched needs a Running parent and an open head. `expected_row_version`, when present, must
     * equal the stored version. The relation epoch increases but is not compared: a progress update never
     * conflicts with a parent publication that did not read the relation set.
     */
    [[nodiscard]] virtual asio::awaitable<ProgramChildRelationOutcome> update_child_relation(
        std::string_view             owner_scope,
        std::string_view             parent_run_id,
        std::uint64_t                expected_parent_fence,
        std::optional<std::uint64_t> expected_row_version,
        ProgramChildRecord           next) = 0;

    /**
     * Atomically publishes the run snapshot, journal head, events, effect outbox,
     * and JavaScript command appends. A throw has the strong exception guarantee.
     * An exact retry is AlreadyPresent.
     */
    [[nodiscard]] virtual asio::awaitable<ProgramTransitionPublishResult> compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication) = 0;
    /** Atomically publishes and acquires, releases, or replaces a dispatch lease. */
    [[nodiscard]] virtual asio::awaitable<ProgramTransitionPublishResult> compare_publish_execution(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) {
        co_return ProgramTransitionPublishResult::Conflict;
    }
    /** Host-only peer for a running source checkpoint; evidence is never serialized. */
    [[nodiscard]] virtual asio::awaitable<ProgramTransitionPublishResult> compare_publish_graph_safe_point(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) {
        co_return ProgramTransitionPublishResult::Conflict;
    }
    /**
     * Atomically advances the ownership fence and the journal head of the Running run `run_id`: publishes the stored
     * record with `fence = expected_fence + 1` and one new journal record, and changes nothing else. Conflict when the
     * run is absent, is not Running, or its head or fence differ from the expected ones.
     */
    [[nodiscard]] virtual asio::awaitable<ProgramTransitionPublishResult> compare_publish_takeover(
        std::string_view owner_scope,
        std::string_view run_id,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence) {
        co_return ProgramTransitionPublishResult::Conflict;
    }
};

/**
 * Base of a ProgramTransitionStore whose backend is synchronous: the in-memory store, SQLite and PostgreSQL. A derived
 * class implements the `..._sync` operations and gets the awaitable interface from them. Each awaitable operation runs
 * the synchronous one at once on the calling thread, or on a thread of a bounded pool when offloads() is true. An
 * operation with a default keeps the default it had.
 */
class NEOGRAPH_PROGRAM_API SyncProgramTransitionStore : public ProgramTransitionStore {
public:
    // The synchronous operations. Each has the contract of the awaitable operation of the same name.
    virtual std::optional<ProgramRunRecord> load_sync(
        std::string_view owner_scope,
        std::string_view run_id) const = 0;
    virtual std::optional<ProgramJournalRecord> latest_sync(
        std::string_view owner_scope,
        std::string_view run_id) const = 0;
    virtual std::optional<ProgramRunPublicationHead> load_run_publication_head_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual std::vector<ProgramEvent> load_events_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const = 0;
    virtual std::vector<ProgramEffectOutboxEntry> load_effects_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const = 0;
    virtual std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    virtual std::optional<ProgramCommandPublicationHead> load_command_publication_head_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual std::vector<ProgramContextPublication> load_context_publications_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    virtual std::vector<HookOutboxEntry> load_hook_outbox_entries_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual ProgramEffectiveRuntimeState load_effective_runtime_state_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual std::vector<ProgramChildSynthesisRecord> load_child_syntheses_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual std::optional<MigrationPlan> load_migration_plan_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*run_id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramRunLineage> load_lineage_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramRunLineage> load_run_lineage_sync(
        std::string_view owner_scope,
        std::string_view run_id) const;
    virtual std::optional<ProgramRunLineage> load_lineage_head_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/,
        std::string_view /*head_id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramRunGeneration> load_generation_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*lineage_id*/,
        std::uint64_t /*generation*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramTransitionPublication> load_generation_initial_publication_sync(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const;
    virtual std::optional<GraphMigrationCapsule> load_graph_migration_capsule_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*source_run_id*/,
        std::string_view /*source_lineage_head_id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramExecutionLease> load_execution_lease_sync(
        std::string_view /*owner_scope*/,
        std::string_view /*run_id*/) const {
        return std::nullopt;
    }
    virtual std::optional<ProgramChildRelationHead> load_child_relation_head_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id) const = 0;
    virtual std::optional<ProgramChildRelation> load_child_relation_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view child_run_id) const = 0;
    virtual std::vector<ProgramChildRelation> load_child_relations_sync(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        bool             open_only) const = 0;
    virtual ProgramChildRelationOutcome insert_child_relation_sync(
        std::string_view   owner_scope,
        std::string_view   parent_run_id,
        std::uint64_t      expected_parent_fence,
        ProgramChildRecord child) = 0;
    virtual ProgramChildRelationOutcome update_child_relation_sync(
        std::string_view             owner_scope,
        std::string_view             parent_run_id,
        std::uint64_t                expected_parent_fence,
        std::optional<std::uint64_t> expected_row_version,
        ProgramChildRecord           next) = 0;
    virtual ProgramTransitionPublishResult compare_publish_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication) = 0;
    virtual ProgramTransitionPublishResult compare_publish_execution_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) {
        return ProgramTransitionPublishResult::Conflict;
    }
    virtual ProgramTransitionPublishResult compare_publish_graph_safe_point_sync(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) {
        return ProgramTransitionPublishResult::Conflict;
    }
    virtual ProgramTransitionPublishResult compare_publish_takeover_sync(
        std::string_view owner_scope,
        std::string_view run_id,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence) {
        return ProgramTransitionPublishResult::Conflict;
    }

    // The awaitable interface, completed by the operations above: at once on the calling thread,
    // or on a thread of a bounded pool when offloads() is true.
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunRecord>> load(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramJournalRecord>> latest(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunPublicationHead>> load_run_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramEvent>> load_events(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramEffectOutboxEntry>> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramJavaScriptCommandJournalEntry>> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramCommandPublicationHead>> load_command_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramContextPublication>> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const final;
    [[nodiscard]] asio::awaitable<std::vector<HookOutboxEntry>> load_hook_outbox_entries(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<ProgramEffectiveRuntimeState> load_effective_runtime_state(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramChildSynthesisRecord>> load_child_syntheses(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<MigrationPlan>> load_migration_plan(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunLineage>> load_lineage(
        std::string_view owner_scope,
        std::string_view lineage_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunLineage>> load_run_lineage(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunLineage>> load_lineage_head(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::string_view head_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramRunGeneration>> load_generation(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramTransitionPublication>> load_generation_initial_publication(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const final;
    [[nodiscard]] asio::awaitable<std::optional<GraphMigrationCapsule>> load_graph_migration_capsule(
        std::string_view owner_scope,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramExecutionLease>> load_execution_lease(
        std::string_view owner_scope,
        std::string_view run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramChildRelationHead>> load_child_relation_head(
        std::string_view owner_scope,
        std::string_view parent_run_id) const final;
    [[nodiscard]] asio::awaitable<std::optional<ProgramChildRelation>> load_child_relation(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view child_run_id) const final;
    [[nodiscard]] asio::awaitable<std::vector<ProgramChildRelation>> load_child_relations(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        bool             open_only) const final;
    [[nodiscard]] asio::awaitable<ProgramChildRelationOutcome> insert_child_relation(
        std::string_view   owner_scope,
        std::string_view   parent_run_id,
        std::uint64_t      expected_parent_fence,
        ProgramChildRecord child) final;
    [[nodiscard]] asio::awaitable<ProgramChildRelationOutcome> update_child_relation(
        std::string_view             owner_scope,
        std::string_view             parent_run_id,
        std::uint64_t                expected_parent_fence,
        std::optional<std::uint64_t> expected_row_version,
        ProgramChildRecord           next) final;
    [[nodiscard]] asio::awaitable<ProgramTransitionPublishResult> compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication) final;
    [[nodiscard]] asio::awaitable<ProgramTransitionPublishResult> compare_publish_execution(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) final;
    [[nodiscard]] asio::awaitable<ProgramTransitionPublishResult> compare_publish_graph_safe_point(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) final;
    [[nodiscard]] asio::awaitable<ProgramTransitionPublishResult> compare_publish_takeover(
        std::string_view owner_scope,
        std::string_view run_id,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence) final;

    /** True when every awaitable operation completes at once on the calling thread, not on a pool thread. */
    bool completes_at_once() const noexcept { return !offloads(); }

protected:
    /** True for a backend that blocks for long: each operation then runs on a pool thread. */
    virtual bool offloads() const noexcept { return false; }
};

/**
 * A ProgramTransitionStore for a caller that cannot suspend: the synchronous Program API, a destructor, a callback. A
 * synchronous store is called directly. Any other store is awaited on a private io_context of the calling thread, unless
 * the policy refuses to wait (BlockingStorePolicy), in which case the call throws ProgramBlockingCallRefused.
 */
class NEOGRAPH_PROGRAM_API BlockingProgramTransitionStore {
public:
    BlockingProgramTransitionStore(std::shared_ptr<ProgramTransitionStore> store, BlockingStorePolicy policy = {});
    /** A view of a store that outlives it. */
    BlockingProgramTransitionStore(ProgramTransitionStore& store, BlockingStorePolicy policy = {});

    const std::shared_ptr<ProgramTransitionStore>& store() const noexcept { return store_; }

    std::string process_coordination_key() const;
    std::optional<ProgramRunRecord> load(std::string_view owner_scope, std::string_view run_id) const;
    std::optional<ProgramJournalRecord> latest(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::optional<ProgramRunPublicationHead> load_run_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::vector<ProgramEvent> load_events(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    std::vector<ProgramEffectOutboxEntry> load_effects(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    std::vector<ProgramJavaScriptCommandJournalEntry> load_javascript_commands(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    std::optional<ProgramCommandPublicationHead> load_command_publication_head(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::vector<ProgramContextPublication> load_context_publications(
        std::string_view owner_scope,
        std::string_view run_id,
        std::uint64_t after_sequence = 0) const;
    std::vector<HookOutboxEntry> load_hook_outbox_entries(
        std::string_view owner_scope,
        std::string_view run_id) const;
    ProgramEffectiveRuntimeState load_effective_runtime_state(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::vector<ProgramChildSynthesisRecord> load_child_syntheses(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::optional<MigrationPlan> load_migration_plan(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::optional<ProgramRunLineage> load_lineage(
        std::string_view owner_scope,
        std::string_view lineage_id) const;
    std::optional<ProgramRunLineage> load_run_lineage(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::optional<ProgramRunLineage> load_lineage_head(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::string_view head_id) const;
    std::optional<ProgramRunGeneration> load_generation(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const;
    std::optional<ProgramTransitionPublication> load_generation_initial_publication(
        std::string_view owner_scope,
        std::string_view lineage_id,
        std::uint64_t generation) const;
    std::optional<GraphMigrationCapsule> load_graph_migration_capsule(
        std::string_view owner_scope,
        std::string_view source_run_id,
        std::string_view source_lineage_head_id) const;
    std::optional<ProgramExecutionLease> load_execution_lease(
        std::string_view owner_scope,
        std::string_view run_id) const;
    std::optional<ProgramChildRelationHead> load_child_relation_head(
        std::string_view owner_scope,
        std::string_view parent_run_id) const;
    std::optional<ProgramChildRelation> load_child_relation(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        std::string_view child_run_id) const;
    std::vector<ProgramChildRelation> load_child_relations(
        std::string_view owner_scope,
        std::string_view parent_run_id,
        bool             open_only = false) const;
    ProgramChildRelationOutcome insert_child_relation(
        std::string_view   owner_scope,
        std::string_view   parent_run_id,
        std::uint64_t      expected_parent_fence,
        ProgramChildRecord child) const;
    ProgramChildRelationOutcome update_child_relation(
        std::string_view             owner_scope,
        std::string_view             parent_run_id,
        std::uint64_t                expected_parent_fence,
        std::optional<std::uint64_t> expected_row_version,
        ProgramChildRecord           next) const;
    ProgramTransitionPublishResult compare_publish(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication) const;
    ProgramTransitionPublishResult compare_publish_execution(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        std::optional<ProgramExecutionLease> expected_lease,
        std::optional<ProgramExecutionLease> next_lease) const;
    ProgramTransitionPublishResult compare_publish_graph_safe_point(
        std::string_view owner_scope,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence,
        ProgramTransitionPublication publication,
        const ProgramGraphSafePointEvidence& evidence,
        const GraphMigrationCapsule& capsule,
        const ProgramExecutionLease& execution_lease) const;
    ProgramTransitionPublishResult compare_publish_takeover(
        std::string_view owner_scope,
        std::string_view run_id,
        std::string_view expected_journal_head,
        std::uint64_t expected_fence) const;

private:
    std::shared_ptr<ProgramTransitionStore> store_;
    SyncProgramTransitionStore* sync_ = nullptr;
    BlockingStorePolicy policy_;
};

class NEOGRAPH_PROGRAM_API InMemoryProgramTransitionStore final
    : public SyncProgramTransitionStore {
public:
    InMemoryProgramTransitionStore();
    ~InMemoryProgramTransitionStore() override;
    InMemoryProgramTransitionStore(InMemoryProgramTransitionStore&&) noexcept;
    InMemoryProgramTransitionStore& operator=(InMemoryProgramTransitionStore&&) noexcept;
    InMemoryProgramTransitionStore(const InMemoryProgramTransitionStore&) = delete;
    InMemoryProgramTransitionStore& operator=(const InMemoryProgramTransitionStore&) = delete;

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
    ProgramTransitionPublishResult
    compare_publish_sync(std::string_view owner_scope,
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

    /// Fail the next publication at a staged boundary; test-only hook.
    void fail_next_publication_for_testing(ProgramTransitionFaultPoint point);

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
