/**
 * @file program/replacement.h
 * @brief Exact durable handoff receipts for Program generation replacement.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/result.h>

#include <asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

class ProgramJavaScriptCommandJournalEntry;
class ProgramHandle;
class ProgramRuntime;
class ProgramRunGeneration;
class ProgramRunLineage;
class ProgramRunRecord;

/** Exact completed JavaScript checkpoint coordinate selected by the host. */
struct ExactProgramHandoffReference {
    std::string source_run_id;
    std::string source_journal_head;
    std::string command_coordinate_id;
    std::string command_entry_id;

    bool operator==(const ExactProgramHandoffReference&) const = default;
};

/** An exact durable handoff reference together with its deeply owned value. */
struct ExactProgramHandoff {
    ExactProgramHandoffReference reference;
    json                         value;
};

/**
 * Move-only host lease holding a source generator at an exact durable checkpoint.
 * Destroying an unconsumed lease lets the source continue from that checkpoint.
 */
class NEOGRAPH_PROGRAM_API ProgramHandoff final {
public:
    ProgramHandoff(const ProgramHandoff&)            = delete;
    ProgramHandoff& operator=(const ProgramHandoff&) = delete;
    ProgramHandoff(ProgramHandoff&& other) noexcept;
    ProgramHandoff& operator=(ProgramHandoff&& other) noexcept;
    ~ProgramHandoff();

    const ExactProgramHandoffReference& reference() const;
    json                                value() const;
    asio::awaitable<ExactProgramHandoff> wait_async() const;
    explicit operator bool() const noexcept;

private:
    struct Impl;
    explicit ProgramHandoff(std::shared_ptr<Impl> impl) noexcept;
    static asio::awaitable<ExactProgramHandoff> wait_async_with_impl(
        std::shared_ptr<Impl> impl);
    void consume() noexcept;

    std::shared_ptr<Impl> impl_;

    friend class ProgramHandle;
    friend class ProgramRuntime;
};

struct ProgramReplacementReceiptData {
    std::string   owner_scope;
    std::string   lineage_id;
    std::uint64_t source_generation = 0;
    std::string   source_generation_id;
    std::string   source_lineage_head_id;
    std::string   source_run_id;
    std::string   source_run_record_id;
    std::string   source_journal_head;
    std::string   source_program_version_id;
    std::string   source_bundle_id;
    std::string   checkpoint_coordinate_id;
    std::string   checkpoint_entry_id;
    std::string   handoff_value_identity;
    std::uint64_t target_generation = 0;
    std::string   target_run_id;
    std::string   target_program_version_id;
    std::string   target_bundle_id;
    std::string   target_input_identity;
    std::string   target_invocation_id;
    std::string   target_binding_fingerprint;
    std::string   target_initial_run_record_id;
    std::string   target_initial_journal_head;
};

NEOGRAPH_PROGRAM_API std::string program_replacement_handoff_identity(const json& value);
NEOGRAPH_PROGRAM_API std::string program_replacement_input_identity(const json& value);

/** Immutable evidence of one host-admitted same-lineage generation transition. */
class NEOGRAPH_PROGRAM_API ProgramReplacementReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramReplacementReceipt(ProgramReplacementReceiptData data);
    static ProgramReplacementReceipt parse(std::string_view stored_bytes);

    const std::string& id() const noexcept;
    const std::string& owner_scope() const noexcept;
    const std::string& lineage_id() const noexcept;
    std::uint64_t source_generation() const noexcept;
    const std::string& source_generation_id() const noexcept;
    const std::string& source_lineage_head_id() const noexcept;
    const std::string& source_run_id() const noexcept;
    const std::string& source_run_record_id() const noexcept;
    const std::string& source_journal_head() const noexcept;
    const std::string& source_program_version_id() const noexcept;
    const std::string& source_bundle_id() const noexcept;
    const std::string& checkpoint_coordinate_id() const noexcept;
    const std::string& checkpoint_entry_id() const noexcept;
    const std::string& handoff_value_identity() const noexcept;
    std::uint64_t target_generation() const noexcept;
    const std::string& target_run_id() const noexcept;
    const std::string& target_program_version_id() const noexcept;
    const std::string& target_bundle_id() const noexcept;
    const std::string& target_input_identity() const noexcept;
    const std::string& target_invocation_id() const noexcept;
    const std::string& target_binding_fingerprint() const noexcept;
    const std::string& target_initial_run_record_id() const noexcept;
    const std::string& target_initial_journal_head() const noexcept;

    bool matches_handoff(const json& value) const;
    bool matches_target_input(const json& value) const;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramReplacementReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Shared fail-closed validator used by every atomic transition backend. */
NEOGRAPH_PROGRAM_API bool is_valid_program_replacement_transition(
    const ProgramRunGeneration&                  predecessor,
    const ProgramRunLineage&                     previous_lineage,
    const ProgramRunRecord&                      source,
    const ProgramJavaScriptCommandJournalEntry& checkpoint,
    const ProgramRunGeneration&                  successor,
    const ProgramRunLineage&                     next_lineage,
    const ProgramRunRecord&                      target) noexcept;

/** Charges elapsed wall time before a successor receives the lineage remainder. */
NEOGRAPH_PROGRAM_API RunBudget program_replacement_remaining_budget(
    const ProgramRunRecord& source,
    const ProgramRunLineage& source_lineage,
    std::int64_t transition_time_ms) noexcept;

}  // namespace neograph::program
