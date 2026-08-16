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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

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
    AfterLineageSnapshot,
    BeforeCommit,
};

class NEOGRAPH_PROGRAM_API ProgramTransitionStore {
public:
    virtual ~ProgramTransitionStore() = default;

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

    /**
     * Atomically publishes the run snapshot, journal head, events, effect outbox,
     * and JavaScript command appends. A throw has the strong exception guarantee.
     * An exact retry is AlreadyPresent.
     */
    virtual ProgramTransitionPublishResult
    compare_publish(std::string_view owner_scope,
                    std::string_view expected_journal_head,
                    ProgramTransitionPublication publication) = 0;
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
    ProgramTransitionPublishResult
    compare_publish(std::string_view owner_scope,
                    std::string_view expected_journal_head,
                    ProgramTransitionPublication publication) override;

    /// Fail the next publication at a staged boundary; test-only hook.
    void fail_next_publication_for_testing(ProgramTransitionFaultPoint point);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
