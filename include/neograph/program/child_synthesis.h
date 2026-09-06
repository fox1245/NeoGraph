/** @file program/child_synthesis.h @brief Durable, generation-scoped child synthesis. */
#pragma once

#include <neograph/program/synthesis.h>

namespace neograph::program {

enum class ProgramChildSynthesisState {
    Reserved,
    Compiling,
    Compiled,
    Validating,
    Validated,
    Admitting,
    Admitted,
    Bound,
    Dispatching,
    Spawned,
    Failed,
    ReconciliationRequired
};

struct ProgramChildSynthesisRecordData {
    ProgramSynthesisProposal    proposal;
    ProgramChildSynthesisGrant  grant;
    ProgramChildSynthesisParent parent;
    ProgramSynthesisReservation reservation;
    std::string                 binding_name;
    std::uint64_t               revision = 1;
    std::string                 previous_id;
    ProgramChildSynthesisState  state = ProgramChildSynthesisState::Reserved;
    /// Cumulative immutable stage outputs; no stage may replace an earlier output.
    json        artifacts = json::object();
    std::string error_code;
};

/** Stored grants are evidence only; recovery must reselect them through host policy. */
class NEOGRAPH_PROGRAM_API ProgramChildSynthesisRecord final {
public:
    static constexpr std::size_t           MAX_RECORD_BYTES = 8 * 1024 * 1024;
    static ProgramChildSynthesisRecord     create(ProgramChildSynthesisRecordData data);
    static ProgramChildSynthesisRecord     parse(std::string_view bytes);
    const ProgramChildSynthesisRecordData& data() const noexcept;
    const std::string&                     id() const noexcept;
    std::string                            serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramChildSynthesisRecord(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Checks only immutable stage history; publication additionally verifies the live debit. */
NEOGRAPH_PROGRAM_API bool is_valid_program_child_synthesis_append(
    const std::vector<ProgramChildSynthesisRecord>& heads,
    const ProgramChildSynthesisRecord&              next) noexcept;

struct ProgramTransitionPublication;
NEOGRAPH_PROGRAM_API bool is_valid_program_child_synthesis_publication(
    const std::vector<ProgramChildSynthesisRecord>& heads,
    const ProgramRunRecord*                         previous_run,
    const ProgramTransitionPublication&             publication) noexcept;

}  // namespace neograph::program
