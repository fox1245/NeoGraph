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

    /**
     * Cancels the run. Cancelling an interrupted run publishes its Cancelled state through the transition store, and
     * the call waits for that. It does not wait on a thread that runs the runtime's executor (a node body) or on a
     * host-pumped executor: there a run that is executing stops in memory at once, and the Cancelled state of an
     * interrupted run is not guaranteed (the call may return false and leave it Interrupted). Use cancel_async, which
     * waits for the Cancelled state and publishes it.
     */
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
    /**
     * The awaitable form of next_handoff (#557): the coordination lock is awaited, not waited for. The returned
     * awaitable keeps the run's control alive, so it may outlive the handle that made it.
     */
    asio::awaitable<ProgramHandoff>       next_handoff_async() const;
    ProgramRunRecord                      snapshot() const;
    /**
     * Awaitable forms of cancel, latest_handoff and snapshot (#557). They await the transition store on the
     * runtime's executor instead of blocking a thread, so they work on a host-pumped executor, where the
     * synchronous forms cannot read a store that completes only in a later turn.
     */
    asio::awaitable<bool>                 cancel_async();
    asio::awaitable<std::optional<ExactProgramHandoff>> latest_handoff_async() const;
    asio::awaitable<ProgramRunRecord>     snapshot_async() const;
    /**
     * Notices about the run, none of them an error. ProgramRuntime::fork gives
     * one when it creates the run: a fork that cannot replay provider calls
     * (RuntimeConfig::core_provider_calls_journaled) names the calls of the
     * paused operation it may send and charge again. A handle from reconnect()
     * or from a repeated fork() of the same run has none of these.
     * A durable provider-call journal reports others as the run executes
     * (#543), on any handle, so read them after wait() to see every one:
     * P_PROVIDER_CALL_POLICY_CHANGED, when a resume or a fork found a call
     * under another SchemaProvider descriptor policy identity and used the
     * journaled outcome, and P_FORK_PROVIDER_CALL_RESENT, when a fork sent a
     * call its source had already journaled because
     * RuntimeConfig::fork_may_resend_provider_calls allows it. A resume reports
     * P_PROVIDER_UPLOAD_RESTARTED when it sent again the call that opens a
     * chunked upload's session, whose URL the journal does not hold (#541).
     * A pricing broker (bind_priced_provider_call, or the journal's
     * bind_priced) reports P_MONEY_OVER_HOLD (#428) for each call it charged
     * more than the reservation it made for the call, which happens when the
     * response reports more than the reservation priced, for example an
     * upstream that returned more output than the request's output cap. The
     * witness is {call_id, operation_id, node, model, evidence,
     * reserved_microunits, microunits, over_by}, with evidence `reported` or
     * `estimated`, and served_by, the upstream that served the call, when the
     * call's ProgramMonetaryRecord has one. The call is charged as reported and the run's status and
     * budgets are as they would be without the notice; a call that a durable
     * journal replays, and a `held` call, give none.
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
