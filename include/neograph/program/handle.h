/**
 * @file program/handle.h
 * @brief Shared control handle for one Program attempt.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/event.h>
#include <neograph/program/replacement.h>
#include <neograph/program/result.h>
#include <neograph/program/run_record.h>

#include <asio/awaitable.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neograph::program {

namespace detail {
class RunControl;
}
class ProgramRuntime;
struct ProgramResume;
struct ProgramEffectResolution;

/**
 * A fact about how the runtime started a run that the caller should know. A
 * notice is not an error: it never changes the run's outcome. It belongs to
 * the handle the call returned and is not stored with the run.
 */
struct ProgramNotice {
    /// Stable identifier, for example P_FORK_PROVIDER_CALLS_NOT_JOURNALED.
    std::string code;
    /// One sentence for a person.
    std::string message;
    /// The facts the message summarizes, as JSON.
    json        witness;

    bool operator==(const ProgramNotice&) const = default;
};

class NEOGRAPH_PROGRAM_API ProgramHandle {
public:
    ProgramHandle(const ProgramHandle&) noexcept            = default;
    ProgramHandle& operator=(const ProgramHandle&) noexcept = default;
    ProgramHandle(ProgramHandle&&) noexcept                 = default;
    ProgramHandle& operator=(ProgramHandle&&) noexcept      = default;
    ~ProgramHandle();

    const std::string& run_id() const noexcept;
    /** Stable agent identity; reconnect this id to inspect its active generation. */
    const std::string& logical_run_id() const noexcept;
    const std::string& program_version_id() const noexcept;
    std::uint64_t      attempt() const noexcept;

    bool                                  cancel() noexcept;
    ProgramResult                         wait() const;
    asio::awaitable<ProgramResult>        wait_async() const;
    std::optional<ProgramResult>          try_result() const;
    std::vector<ProgramEvent>             events_after(std::uint64_t sequence) const;
    std::optional<CoreCheckpointIdentity> latest_checkpoint() const;
    /** Latest completed top-level ng.checkpoint that still owns the lineage head. */
    std::optional<ExactProgramHandoff>    latest_handoff() const;
    /** Hold the source before it consumes the next completed top-level checkpoint. */
    ProgramHandoff                        next_handoff() const;
    ProgramRunRecord                      snapshot() const;
    /**
     * Notices the runtime gave when the call that returned this handle
     * created the run. Only ProgramRuntime::fork gives one: a fork that
     * cannot replay provider calls (RuntimeConfig::core_provider_calls_journaled)
     * names the calls of the paused operation it may send and charge again.
     * Empty for every other handle, including one from reconnect() or from
     * a repeated fork() of the same run.
     */
    std::vector<ProgramNotice>            notices() const;
    ProgramHandle resume(ProgramRuntime& runtime, ProgramResume resume) const;
    ProgramHandle reconcile(ProgramRuntime& runtime,
                            ProgramEffectResolution resolution) const;

private:
    explicit ProgramHandle(std::shared_ptr<detail::RunControl> control);
    std::shared_ptr<detail::RunControl> control_;

    friend class ProgramRuntime;
};

}  // namespace neograph::program
