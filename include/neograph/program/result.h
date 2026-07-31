/**
 * @file program/result.h
 * @brief Immutable outcome of one Program run attempt.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/program/pending.h>

#include <cstdint>
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
    std::uint32_t max_child_depth        = 0;
    std::uint64_t max_total_children     = 0;

    bool operator==(const RunBudget&) const = default;
};

struct ProgramUsage {
    std::uint64_t wall_time_ms        = 0;
    std::uint64_t model_tokens        = 0;
    std::uint64_t monetary_microunits = 0;
    std::uint64_t program_operations  = 0;
    std::uint64_t core_steps          = 0;
    std::uint32_t peak_concurrency    = 0;

    bool operator==(const ProgramUsage&) const = default;
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

    bool operator==(const ProgramFailure&) const = default;
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
    std::optional<ProgramFailure>         failure() const;
    std::vector<std::string>              execution_trace() const;

private:
    using ConstructionData = ProgramResultData;

    struct Impl;
    explicit ProgramResult(ConstructionData data);
    std::shared_ptr<const Impl> impl_;

    friend class detail::RunControl;
};

}  // namespace neograph::program
