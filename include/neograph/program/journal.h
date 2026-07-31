/**
 * @file program/journal.h
 * @brief Atomic, content-addressed Program continuation journal.
 */
#pragma once

#include <neograph/program/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

enum class ContinuationState : std::uint8_t {
    Running,
    Interrupted,
    Completed,
    Cancelled,
    BudgetExhausted,
    TimedOut,
    Failed,
    AmbiguousEffect,
    CheckpointIncompatible,
};

NEOGRAPH_PROGRAM_API std::string_view  to_string(ContinuationState state) noexcept;
NEOGRAPH_PROGRAM_API ContinuationState continuation_state_from_string(std::string_view value);

struct ProgramContinuation {
    std::string       operation_id;
    ContinuationState state   = ContinuationState::Running;
    std::uint64_t     attempt = 0;

    bool operator==(const ProgramContinuation&) const = default;
};

/**
 * Mutable construction input for a journal record. ProgramJournalRecord::create validates and
 * seals this data with a deterministic content identity.
 */
struct ProgramJournalRecordData {
    std::string                           previous_id;
    std::string                           run_id;
    std::string                           program_version_id;
    std::string                           bundle_id;
    std::uint64_t                         sequence = 0;
    ProgramContinuation                   continuation;
    RunBudget                             remaining_budget{};
    RunBudget                             inflight_reservation{};
    std::optional<CoreCheckpointIdentity> core_checkpoint;
    std::int64_t                          timestamp_ms = 0;
};

/**
 * Deep-owned stored journal value. Public fields preserve the v1 wire/value shape; use create()
 * or parse() to obtain a sealed record. ProgramJournal::compare_append rejects mutations that
 * make the id disagree with the canonical body.
 */
struct NEOGRAPH_PROGRAM_API ProgramJournalRecord {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    std::string                           id;
    std::string                           previous_id;
    std::string                           run_id;
    std::string                           program_version_id;
    std::string                           bundle_id;
    std::uint64_t                         sequence = 0;
    ProgramContinuation                   continuation;
    RunBudget                             remaining_budget{};
    RunBudget                             inflight_reservation{};
    std::optional<CoreCheckpointIdentity> core_checkpoint;
    std::int64_t                          timestamp_ms = 0;

    static ProgramJournalRecord create(ProgramJournalRecordData data);
    static ProgramJournalRecord parse(std::string_view stored_bytes);

    /** Canonical stored representation, including id. Throws if the public value was mutated. */
    std::string serialize_canonical() const;
};

enum class JournalAppendResult : std::uint8_t { Appended, AlreadyPresent, Conflict };

class NEOGRAPH_PROGRAM_API ProgramJournal {
public:
    virtual ~ProgramJournal() = default;

    virtual std::optional<ProgramJournalRecord> latest(std::string_view run_id) const = 0;

    /**
     * The sole writer primitive. The expected id, record.previous_id, latest record, sequence,
     * state transition, identities, and budgets are checked in one atomic operation.
     */
    virtual JournalAppendResult compare_append(std::string_view     expected_previous_id,
                                               ProgramJournalRecord record) = 0;
};

/** Shared transition predicate used by atomic Program publication backends. */
NEOGRAPH_PROGRAM_API bool
is_valid_program_journal_transition(const ProgramJournalRecord& previous,
                                    const ProgramJournalRecord& next) noexcept;

class NEOGRAPH_PROGRAM_API InMemoryProgramJournal final : public ProgramJournal {
public:
    InMemoryProgramJournal();
    ~InMemoryProgramJournal() override;

    InMemoryProgramJournal(const InMemoryProgramJournal&)            = delete;
    InMemoryProgramJournal& operator=(const InMemoryProgramJournal&) = delete;
    InMemoryProgramJournal(InMemoryProgramJournal&&) noexcept;
    InMemoryProgramJournal& operator=(InMemoryProgramJournal&&) noexcept;

    std::optional<ProgramJournalRecord> latest(std::string_view run_id) const override;
    JournalAppendResult                 compare_append(std::string_view     expected_previous_id,
                                                       ProgramJournalRecord record) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
