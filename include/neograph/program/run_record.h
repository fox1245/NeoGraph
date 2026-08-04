/**
 * @file program/run_record.h
 * @brief Canonical immutable snapshot of one owner-scoped Program run.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/fork.h>
#include <neograph/program/journal.h>
#include <neograph/program/pending.h>
#include <neograph/program/invocation.h>
#include <neograph/program/result.h>

#include <vector>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

struct ProgramPersistedInvocation {
    json        input = json::object();
    RunBudget   granted_budget;
    std::string trace_id;
    std::string parent_run_id;
    std::uint32_t child_depth = 0;

    bool operator==(const ProgramPersistedInvocation&) const = default;
};

enum class ProgramChildState : std::uint8_t {
    Publishing,
    Dispatched,
    Completed,
    Cancelled,
    Failed,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramChildState state) noexcept;
NEOGRAPH_PROGRAM_API ProgramChildState program_child_state_from_string(std::string_view value);

struct ProgramChildRecord {
    std::string                child_run_id;
    std::string                link_id;
    std::string                link_receipt;
    ProgramPersistedInvocation invocation;
    ProgramChildState          state = ProgramChildState::Publishing;
    /** Terminal outcome published with the parent join record, when available. */
    std::optional<ProgramResult> terminal_result;

    bool operator==(const ProgramChildRecord&) const = default;
};

struct ProgramRunRecordData {
    std::string                           owner_scope;
    std::string                           run_id;
    std::string                           program_version_id;
    std::string                           bundle_id;
    std::string                           binding_fingerprint;
    RunInvocation                        invocation;
    /// Runtime-only topology metadata; request identity remains in invocation.
    std::uint32_t                         child_depth = 0;
    ProgramContinuation                   continuation;
    RunBudget                             remaining_budget;
    std::optional<CoreCheckpointIdentity> exact_checkpoint;
    std::optional<ProgramPendingInput>     pending_input;
    std::optional<ProgramPendingEffect>    pending_effect;
    std::optional<ProgramResult>           terminal_result;
    std::optional<ForkCompatibilityReceipt> fork_receipt;
    std::vector<ProgramChildRecord>           children;
    std::string                           journal_head;
    std::optional<std::string>              fork_source_run_id;
    std::optional<std::string>              fork_source_program_version_id;
    std::optional<std::string>              fork_source_checkpoint_id;
    std::optional<std::string>              recorded_binding_set_fingerprint;
    std::uint64_t                         event_sequence  = 0;
    std::uint64_t                         effect_sequence = 0;
    std::int64_t                          created_at_ms    = 0;
    std::int64_t                          updated_at_ms    = 0;
};

/** Deep-owned, content-addressed run snapshot used for reconnect and CAS publication. */
class NEOGRAPH_PROGRAM_API ProgramRunRecord {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 2;

    static ProgramRunRecord create(ProgramRunRecordData data);
    static ProgramRunRecord parse(std::string_view stored_bytes);

    const std::string& owner_scope() const noexcept;
    const std::string& run_id() const noexcept;
    const std::string& program_version_id() const noexcept;
    const std::string& bundle_id() const noexcept;
    const std::string& binding_fingerprint() const noexcept;
    const RunInvocation& invocation() const noexcept;
    std::uint32_t child_depth() const noexcept;
    ProgramContinuation continuation() const noexcept;
    RunBudget remaining_budget() const noexcept;
    std::optional<CoreCheckpointIdentity> exact_checkpoint() const;
    std::optional<ProgramPendingInput> pending_input() const;
    std::optional<ProgramPendingEffect> pending_effect() const;
    std::optional<ProgramResult> terminal_result() const;
    std::vector<ProgramChildRecord> children() const;
    std::optional<ForkCompatibilityReceipt> fork_receipt() const;
    const std::string& journal_head() const noexcept;
    std::uint64_t event_sequence() const noexcept;
    std::uint64_t effect_sequence() const noexcept;
    std::int64_t created_at_ms() const noexcept;
    std::int64_t updated_at_ms() const noexcept;
    const std::string& id() const noexcept;

    std::optional<std::string> fork_source_run_id() const;
    std::optional<std::string> fork_source_program_version_id() const;
    std::optional<std::string> fork_source_checkpoint_id() const;
    std::optional<std::string> recorded_binding_set_fingerprint() const;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramRunRecord(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
