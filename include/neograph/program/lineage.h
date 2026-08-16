/**
 * @file program/lineage.h
 * @brief Immutable run-generation identity and lineage budget heads.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/replacement.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

class ProgramRunRecord;

/** Derives the stable owner-scoped lineage identity for a root run. */
NEOGRAPH_PROGRAM_API std::string program_run_lineage_id(std::string_view owner_scope,
                                                        std::string_view root_run_id);

struct ProgramRunGenerationData {
    std::string                owner_scope;
    std::string                lineage_id;
    std::uint64_t              generation = 0;
    std::string                run_id;
    std::string                program_version_id;
    std::string                bundle_id;
    std::string                initial_run_record_id;
    std::string                initial_journal_head;
    std::optional<std::string> predecessor_generation_id;
    std::int64_t               created_at_ms = 0;
    std::uint32_t              child_depth   = 0;
    std::optional<ProgramReplacementReceipt> replacement_receipt;
};

/** Immutable identity of one admitted topology generation in a run lineage. */
class NEOGRAPH_PROGRAM_API ProgramRunGeneration final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    static ProgramRunGeneration create(ProgramRunGenerationData data);
    static ProgramRunGeneration parse(std::string_view stored_bytes);

    const std::string&                owner_scope() const noexcept;
    const std::string&                lineage_id() const noexcept;
    std::uint64_t                     generation() const noexcept;
    const std::string&                run_id() const noexcept;
    const std::string&                program_version_id() const noexcept;
    const std::string&                bundle_id() const noexcept;
    const std::string&                initial_run_record_id() const noexcept;
    const std::string&                initial_journal_head() const noexcept;
    const std::optional<std::string>& predecessor_generation_id() const noexcept;
    std::int64_t                      created_at_ms() const noexcept;
    std::uint32_t                     child_depth() const noexcept;
    std::optional<ProgramReplacementReceipt> replacement_receipt() const;
    const std::string&                id() const noexcept;
    std::string                       serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramRunGeneration(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramRunLineageData {
    std::string                owner_scope;
    std::string                lineage_id;
    std::string                root_run_id;
    std::uint64_t              active_generation = 0;
    std::string                active_generation_id;
    std::string                active_run_record_id;
    std::string                active_journal_head;
    RunBudget                  remaining_budget;
    RunBudget                  inflight_reservation;
    std::optional<std::string> predecessor_head_id;
    std::int64_t               created_at_ms = 0;
    std::int64_t               updated_at_ms = 0;
    /// Permanently allocated child subtrees; this ledger never decreases.
    RunBudget                  committed_descendant_budget;
};

/** Immutable content-addressed head of one stable run lineage. */
class NEOGRAPH_PROGRAM_API ProgramRunLineage final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ProgramRunLineage create(ProgramRunLineageData data);
    static ProgramRunLineage parse(std::string_view stored_bytes);

    const std::string&                owner_scope() const noexcept;
    const std::string&                lineage_id() const noexcept;
    const std::string&                root_run_id() const noexcept;
    std::uint64_t                     active_generation() const noexcept;
    const std::string&                active_generation_id() const noexcept;
    const std::string&                active_run_record_id() const noexcept;
    const std::string&                active_journal_head() const noexcept;
    RunBudget                         remaining_budget() const noexcept;
    RunBudget                         inflight_reservation() const noexcept;
    const std::optional<std::string>& predecessor_head_id() const noexcept;
    std::int64_t                      created_at_ms() const noexcept;
    std::int64_t                      updated_at_ms() const noexcept;
    RunBudget                         committed_descendant_budget() const noexcept;
    const std::string&                id() const noexcept;
    std::string                       serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramRunLineage(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Validates the first durable head of a lineage against generation one. */
NEOGRAPH_PROGRAM_API bool is_valid_program_run_lineage_initial(
    const ProgramRunLineage& lineage, const ProgramRunGeneration& generation) noexcept;

/** Validates the active generation's immutable binding to a run snapshot. */
NEOGRAPH_PROGRAM_API bool does_program_run_generation_bind(
    const ProgramRunGeneration& generation,
    const ProgramRunLineage& lineage,
    const ProgramRunRecord& run) noexcept;

/**
 * Validates a same-generation head update or an exact successor transition.
 * The combined available and reserved budget may never increase.
 */
NEOGRAPH_PROGRAM_API bool is_valid_program_run_lineage_transition(
    const ProgramRunLineage&                   previous,
    const ProgramRunLineage&                   next,
    const std::optional<ProgramRunGeneration>& successor = std::nullopt) noexcept;

}  // namespace neograph::program
