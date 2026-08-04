/**
 * @file a2a/program_adapter.h
 * @brief ProgramRuntime-backed A2A collaboration adapter.
 *
 * This adapter is intentionally a projection over ProgramRuntime. It owns no
 * executor, Core, or mutable control VM of its own: admission, capability and
 * effect checks stay in ProgramRuntime and the collaboration mailbox narrows
 * the request to the accepted link.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/a2a/collaboration.h>
#include <neograph/a2a/types.h>
#include <neograph/program/runtime.h>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::a2a {

/// Authentication/attenuation failure while admitting a Program-backed A2A request.
class NEOGRAPH_API ProgramA2ARequestError final : public std::runtime_error {
public:
    explicit ProgramA2ARequestError(std::string message)
        : std::runtime_error(std::move(message)) {}
};

/**
 * One admitted ProgramVersion exposed through A2A. The default invocation is
 * `{ "prompt": <text> }` with a conservative policy-bounded budget. Callers
 * with a different input contract should supply an invocation builder.
 */
class NEOGRAPH_API ProgramAgentAdapter final {
public:
    using InvocationBuilder = std::function<program::ProgramInvocation(
        const Message&, std::string_view task_id, std::string_view context_id)>;
    using ArtifactBuilder = std::function<std::optional<Artifact>(
        const program::ProgramResult&, std::string_view task_id)>;

    ProgramAgentAdapter(std::shared_ptr<program::ProgramRuntime> runtime,
                         program::ProgramVersion version,
                         std::string owner_scope,
                         std::shared_ptr<CollaborationMailbox> mailbox = {},
                         InvocationBuilder invocation_builder = {},
                         ArtifactBuilder artifact_builder = {});

    const std::string& owner_scope() const noexcept;
    const program::ProgramVersion& version() const noexcept;
    const std::shared_ptr<program::ProgramRuntime>& runtime() const noexcept;
    const std::shared_ptr<CollaborationMailbox>& mailbox() const noexcept;

    /// Admit and start one request through the existing ProgramRuntime.
    /// A repeated collaboration idempotency key reconnects the exact run.
    program::ProgramHandle start(const Message& inbound,
                                 std::string_view task_id,
                                 std::string_view context_id) const;

    /**
     * Recover accepted, typed mailbox requests for this exact ProgramVersion.
     *
     * The mailbox is the durable inbound-publication boundary. For every
     * unacknowledged request, this either reconnects its exact persisted run
     * or atomically claims the requested run ID with the persisted invocation.
     * It never rebuilds an invocation from a message or acknowledges a record.
     * Records for another admitted ProgramVersion are left for that adapter.
     */
    std::vector<program::ProgramHandle> recover_pending() const;

    /// Reconnect a run from the ProgramTransitionStore without replaying input.
    program::ProgramHandle reconnect(std::string_view task_id) const;

    /// Project a live or terminal Program handle into an A2A Task. The output
    /// and artifact identifiers are deterministic, so this projection is safe
    /// to rebuild after a dropped stream or server restart.
    Task task_snapshot(const program::ProgramHandle& handle,
                       std::string_view task_id,
                       std::string_view context_id) const;

    /// Reconnect and immediately project a durable run. Throws if no durable
    /// Program run exists for this owner/task identity.
    Task reconnect_task(std::string_view task_id,
                        std::string_view context_id = {}) const;

    /// A terminal projection acknowledges the accepted mailbox record, if any.
    void acknowledge_task(std::string_view task_id) const;

private:
    std::shared_ptr<program::ProgramRuntime> runtime_;
    program::ProgramVersion                 version_;
    std::string                             owner_scope_;
    std::shared_ptr<CollaborationMailbox>   mailbox_;
    InvocationBuilder                       invocation_builder_;
    ArtifactBuilder                         artifact_builder_;
};

/// Descriptive alias for callers that name adapters after the transport.
using ProgramA2AAdapter = ProgramAgentAdapter;

}  // namespace neograph::a2a
