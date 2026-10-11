/**
 * @file program/result.h
 * @brief Immutable outcome of one Program run attempt.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/program/pending.h>
#include <neograph/types.h>
#include <core/native_archive.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

namespace detail {
class RunControl;
}

enum class ProgramTerminalStatus : std::uint8_t {
    Completed,
    Interrupted,
    Cancelled,
    BudgetExhausted,
    TimedOut,
    Failed,
    AmbiguousEffect,
    CheckpointIncompatible,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramTerminalStatus status) noexcept;
NEOGRAPH_PROGRAM_API ProgramTerminalStatus
program_terminal_status_from_string(std::string_view value);

struct RunBudget {
    std::uint64_t wall_time_ms           = 0;
    std::uint64_t model_tokens           = 0;
    std::uint64_t monetary_microunits    = 0;
    std::uint32_t max_concurrency        = 0;
    std::uint64_t max_program_operations = 0;
    std::uint64_t max_core_steps         = 0;
    std::uint64_t max_dynamic_compiles   = 0;
    /// Remaining descendant levels granted to this run; each child receives at most grant - 1.
    std::uint32_t max_child_depth = 0;
    /// Reserved descendants, including the child subtree grant, for this run.
    std::uint64_t max_total_children = 0;
    /// #364: run-level ceiling on node retries across every Core call, node,
    /// fan-out branch, Send and subgraph of the run. Absent means none.
    /// Reaching it stops retrying with kind `ceiling`. Serialized only when set.
    std::optional<std::uint64_t> max_node_retries;

    bool operator==(const RunBudget&) const = default;
};

struct ProgramUsage {
    std::uint64_t wall_time_ms        = 0;
    std::uint64_t model_tokens        = 0;
    /// Micro-USD charged to this attempt by priced provider calls: settled
    /// amounts plus the reservations of calls whose outcome is unknown.
    std::uint64_t monetary_microunits = 0;
    std::uint64_t program_operations  = 0;
    std::uint64_t core_steps          = 0;
    std::uint32_t peak_concurrency    = 0;
    /// #364: node retries taken under RunBudget::max_node_retries; counted
    /// only when the run has that ceiling. Serialized only when nonzero.
    std::uint64_t node_retries        = 0;

    bool operator==(const ProgramUsage&) const = default;
};

/** Where the amount of one priced provider call came from. */
enum class ProgramMonetaryEvidence : std::uint8_t {
    /// The provider's own cost for the call (OpenRouter `usage.cost`).
    Reported,
    /// The price catalog applied to the call's reported token usage.
    Estimated,
    /// The outcome or its usage is unknown, so the call's reservation is charged.
    Held,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramMonetaryEvidence evidence) noexcept;
NEOGRAPH_PROGRAM_API ProgramMonetaryEvidence
program_monetary_evidence_from_string(std::string_view value);

/** One provider call of a Program attempt, priced in micro-USD (1e-6 USD). */
struct ProgramMonetaryRecord {
    /// Stable logical call slot, the provider journal's dispatch id.
    std::string             call_id;
    std::string             operation_id;
    /// Core node that made the call.
    std::string             node;
    /// Price catalog origin, for example "openrouter".
    std::string             origin;
    std::string             model;
    ProgramMonetaryEvidence evidence = ProgramMonetaryEvidence::Estimated;
    /// Amount charged to the run.
    std::uint64_t           microunits = 0;
    /// Preflight reservation; 0 when none could be computed.
    std::uint64_t           reserved_microunits = 0;
    /// "provider_reported", "list_rates" or "route_ceiling".
    std::string             basis;
    /// The catalog that priced an estimated or held amount.
    std::string             catalog_version;
    std::string             catalog_digest;
    /// Some tokens were priced at the highest rate they could have billed at.
    bool                    conservative = false;
    /// An aggregator estimate at the list price: the served endpoint may have cost more.
    bool                    may_undercount = false;
    /// Restored from the provider journal after a restart; its request was
    /// sent once, before the restart.
    bool                    replayed = false;
    /// Why an amount is held (the usage state or the unknown outcome).
    std::string             reason;

    bool operator==(const ProgramMonetaryRecord&) const = default;
};

struct CoreCheckpointIdentity {
    std::string   core_name;
    std::string   core_generation_id;
    std::string   core_thread_id;
    std::string   checkpoint_id;
    std::uint32_t checkpoint_schema_version;

    bool operator==(const CoreCheckpointIdentity&) const = default;
};

struct ProgramFailure {
    std::string   code;
    std::string   message;
    std::string   operation_id;
    std::string   core_node;
    std::uint32_t attempts = 0;
    json          witness;
    /// Genuine immutable transport outcome and original live failure cause.
    /// The witness carries full durable evidence; exception_ptr is never stored.
    sp::runtime::Result provider_outcome;
    std::exception_ptr provider_cause;

    bool operator==(const ProgramFailure& other) const {
        return code == other.code && message == other.message &&
            operation_id == other.operation_id && core_node == other.core_node &&
            attempts == other.attempts && witness == other.witness;
    }
};

struct ProgramInterrupt {
    std::string core_node;
    json        value;
    std::optional<ProgramPendingInput>  pending_input;
    std::optional<ProgramPendingEffect> pending_effect;

};

struct ProgramResultData {
    ProgramTerminalStatus                 status = ProgramTerminalStatus::Failed;
    std::string                           run_id;
    std::string                           program_version_id;
    std::string                           bundle_id;
    std::string                           operation_id = "root";
    std::uint64_t                         attempt      = 0;
    json                                  output       = json::object();
    ProgramUsage                          usage;
    RunBudget                             remaining_budget;
    std::optional<CoreCheckpointIdentity> checkpoint;
    std::optional<ProgramInterrupt>       interrupt;
    std::optional<ProgramFailure>         failure;
    std::vector<std::string>              execution_trace;
    /// Actual bank custody, authenticated by the enclosing owner-scoped run journal.
    /// Reported usage is separate from charged credit and UnknownHold reservations.
    std::optional<UsageAccumulator::AuthoritySnapshot> provider_budget_authority;
    /// Priced provider calls of this attempt, in settlement order.
    std::vector<ProgramMonetaryRecord> monetary_records;
};

class NEOGRAPH_PROGRAM_API ProgramResult {
public:
    /**
     * @brief Construct an empty failed value for Asio completion-token interoperability.
     *
     * Asio's `co_spawn` exception path requires the awaited value type to be default
     * constructible even though it delivers the exception instead of this sentinel.
     * Normal Runtime completion never returns this value.
     */
    ProgramResult();
    static ProgramResult create(ProgramResultData data);
    /// Stored native provider failures remain data-only until the owner Runtime
    /// restores their configured archive; failure() rejects an unresolved seal.
    static ProgramResult parse(std::string_view stored_bytes);
    std::string          serialize_canonical() const;
    const std::string&   id() const noexcept;
    ProgramTerminalStatus                 status() const noexcept;
    const std::string&                    run_id() const noexcept;
    const std::string&                    program_version_id() const noexcept;
    const std::string&                    bundle_id() const noexcept;
    const std::string&                    operation_id() const noexcept;
    std::uint64_t                         attempt() const noexcept;
    json                                  output() const;
    ProgramUsage                          usage() const noexcept;
    RunBudget                             remaining_budget() const noexcept;
    std::optional<CoreCheckpointIdentity> checkpoint() const;
    std::optional<ProgramInterrupt>       interrupt() const;
    /// Returns genuine owned provider evidence when present, never a native projection.
    std::optional<ProgramFailure>         failure() const;
    std::vector<std::string>              execution_trace() const;
    const std::optional<UsageAccumulator::AuthoritySnapshot>& provider_budget_authority() const noexcept;
    /// Priced provider calls of this attempt, in settlement order. Empty when
    /// no call went through a pricing broker.
    const std::vector<ProgramMonetaryRecord>& monetary_records() const noexcept;

private:
    using ConstructionData = ProgramResultData;

    struct Impl;
    explicit ProgramResult(ConstructionData data, bool persistable = true);
    ProgramResult restore_provider_failure(
        std::string_view owner_scope, const std::shared_ptr<sp::NativeArchive>& archive) const;
    static ProgramResult create_for_runtime(
        ConstructionData data, std::string_view owner_scope,
        const std::shared_ptr<sp::NativeArchive>& archive);
    std::shared_ptr<const Impl> impl_;

    friend class detail::RunControl;
};

}  // namespace neograph::program
