#include <neograph/a2a/program_adapter.h>

#include <neograph/program/diagnostic.h>

#include <algorithm>
#include <utility>
#include <unordered_set>

namespace neograph::a2a {
namespace {

std::string extract_text(const Message& message) {
    std::string result;
    for (const auto& part : message.parts) {
        if (part.kind != "text") continue;
        if (!result.empty()) result.push_back(' ');
        result += part.text;
    }
    return result;
}

program::RunBudget default_budget(const program::ProgramVersion& version) {
    const auto ceiling = version.policy_snapshot().budget_ceiling();
    const auto bounded = [](std::uint64_t value, std::uint64_t fallback) {
        if (!value) return fallback;
        return std::min(value, fallback);
    };
    program::RunBudget budget;
    budget.wall_time_ms = bounded(ceiling.wall_time_ms, 10'000);
    budget.model_tokens = std::min(ceiling.model_tokens, std::uint64_t{1'000});
    budget.monetary_microunits = std::min(ceiling.monetary_microunits, std::uint64_t{1'000});
    budget.max_concurrency = static_cast<std::uint32_t>(bounded(ceiling.max_concurrency, 1));
    budget.max_program_operations = bounded(ceiling.max_program_operations, 1);
    budget.max_core_steps = bounded(ceiling.max_core_steps, 20);
    // The adapter never grants dynamic compilation or child composition. A
    // custom builder may request these only when its admitted contract allows it.
    budget.max_dynamic_compiles = 0;
    budget.max_child_depth = 0;
    budget.max_total_children = 0;
    return budget;
}

std::optional<program::InvocationArtifactReference>
invocation_artifact(const std::optional<CollaborationEnvelope>& envelope) {
    if (!envelope || envelope->artifacts.empty()) return std::nullopt;
    if (envelope->artifacts.size() != 1) {
        throw ProgramA2ARequestError(
            "A Program-backed collaboration request may carry at most one invocation artifact");
    }
    const auto& artifact = envelope->artifacts.front();
    return program::InvocationArtifactReference{
        artifact.artifact_identity, artifact.uri, artifact.media_type, artifact.size_bytes};
}

program::RunInvocation default_invocation(
    const program::ProgramVersion& version,
    std::string_view owner_scope,
    std::string_view agent_id,
    const std::optional<CollaborationEnvelope>& envelope,
    const Message& inbound,
    std::string_view task_id,
    std::string_view context_id) {
    program::RunInvocation invocation;
    invocation.owner_scope = std::string(owner_scope);
    invocation.agent_id = std::string(agent_id);
    invocation.program_version_id = version.id();
    invocation.run_id = std::string(task_id);
    invocation.budget = default_budget(version);
    invocation.input =
        envelope ? envelope->payload : json{{"prompt", extract_text(inbound)}};
    invocation.message_sequence = envelope ? envelope->sequence : 1;
    invocation.idempotency_key =
        envelope ? envelope->idempotency_key : "legacy-a2a:" + std::string(task_id);
    invocation.correlation_id =
        envelope ? envelope->correlation_id : std::string(context_id);
    invocation.artifact = invocation_artifact(envelope);
    return invocation;
}

Message result_message(const program::ProgramResult& result,
                       std::string_view task_id,
                       std::string_view context_id) {
    Message message;
    message.message_id = "program-result:" + result.id();
    message.role = Role::Agent;
    message.task_id = std::string(task_id);
    message.context_id = std::string(context_id);
    std::string text;
    if (result.status() == program::ProgramTerminalStatus::Completed) {
        text = result.output().is_string() ? result.output().get<std::string>()
                                           : result.output().dump();
    } else if (const auto failure = result.failure()) {
        text = failure->message;
    } else {
        text = std::string(program::to_string(result.status()));
    }
    message.parts.push_back(Part::text_part(std::move(text)));
    return message;
}

TaskState task_state(program::ProgramTerminalStatus status) {
    switch (status) {
        case program::ProgramTerminalStatus::Completed:
            return TaskState::Completed;
        case program::ProgramTerminalStatus::Cancelled:
            return TaskState::Canceled;
        case program::ProgramTerminalStatus::Interrupted:
            return TaskState::InputRequired;
        case program::ProgramTerminalStatus::BudgetExhausted:
        case program::ProgramTerminalStatus::TimedOut:
        case program::ProgramTerminalStatus::Failed:
        case program::ProgramTerminalStatus::AmbiguousEffect:
        case program::ProgramTerminalStatus::CheckpointIncompatible:
            return TaskState::Failed;
    }
    return TaskState::Failed;
}

TaskState continuation_state(program::ContinuationState state) {
    if (state == program::ContinuationState::Interrupted) return TaskState::InputRequired;
    if (state == program::ContinuationState::Running) return TaskState::Working;
    switch (state) {
        case program::ContinuationState::Completed:
            return TaskState::Completed;
        case program::ContinuationState::Cancelled:
            return TaskState::Canceled;
        case program::ContinuationState::BudgetExhausted:
        case program::ContinuationState::TimedOut:
        case program::ContinuationState::Failed:
        case program::ContinuationState::AmbiguousEffect:
        case program::ContinuationState::CheckpointIncompatible:
            return TaskState::Failed;
        case program::ContinuationState::Running:
        case program::ContinuationState::Interrupted:
            break;
    }
    return TaskState::Failed;
}

Artifact default_artifact(const program::ProgramResult& result, std::string_view task_id) {
    Part part;
    part.kind = "data";
    part.data = json{{"format", "neograph-program-result-v1"},
                     {"result_id", result.id()},
                     {"program_version_id", result.program_version_id()},
                     {"status", std::string(program::to_string(result.status()))},
                     {"value", result.output()}};
    Artifact artifact;
    artifact.artifact_id = "program-result:" + std::string(task_id);
    artifact.name = "neograph-program-result";
    artifact.parts.push_back(std::move(part));
    artifact.metadata = json{{"reconnect_safe", true}, {"result_id", result.id()}};
    return artifact;
}

}  // namespace

ProgramAgentAdapter::ProgramAgentAdapter(
    std::shared_ptr<program::ProgramRuntime> runtime,
    program::ProgramVersion version,
    std::string owner_scope,
    std::shared_ptr<CollaborationMailbox> mailbox,
    InvocationBuilder invocation_builder,
    ArtifactBuilder artifact_builder)
    : runtime_(std::move(runtime)),
      version_(std::move(version)),
      owner_scope_(std::move(owner_scope)),
      mailbox_(std::move(mailbox)),
      invocation_builder_(std::move(invocation_builder)),
      artifact_builder_(std::move(artifact_builder)) {
    if (!runtime_) throw std::invalid_argument("ProgramAgentAdapter runtime is null");
    if (owner_scope_.empty()) throw std::invalid_argument("ProgramAgentAdapter owner is required");
    if (version_.ownership_scope() != owner_scope_) {
        throw std::invalid_argument("ProgramAgentAdapter version owner mismatch");
    }
    if (mailbox_ && (mailbox_->owner_scope() != owner_scope_ || mailbox_->agent_id().empty())) {
        throw std::invalid_argument("ProgramAgentAdapter mailbox owner mismatch");
    }
}

const std::string& ProgramAgentAdapter::owner_scope() const noexcept { return owner_scope_; }
const program::ProgramVersion& ProgramAgentAdapter::version() const noexcept { return version_; }
const std::shared_ptr<program::ProgramRuntime>& ProgramAgentAdapter::runtime() const noexcept {
    return runtime_;
}
const std::shared_ptr<CollaborationMailbox>& ProgramAgentAdapter::mailbox() const noexcept {
    return mailbox_;
}

program::ProgramHandle ProgramAgentAdapter::start(const Message& inbound,
                                                   std::string_view task_id,
                                                   std::string_view context_id) const {
    if (task_id.empty() || context_id.empty()) {
        throw ProgramA2ARequestError("Program-backed A2A task and context IDs are required");
    }
    std::optional<CollaborationEnvelope> envelope;
    bool collaboration_marked = inbound.metadata.is_object() &&
                                 inbound.metadata.value("neograph_collaboration", false);
    for (const auto& part : inbound.parts) {
        if (part.kind == "data" && part.data.is_object() &&
            part.data.value("format", std::string()) == "neograph-a2a-collaboration-v1") {
            collaboration_marked = true;
        }
    }
    try {
        envelope = collaboration_from_message(inbound);
    } catch (const std::invalid_argument&) {
        if (collaboration_marked) {
            throw ProgramA2ARequestError("Malformed collaboration envelope");
        }
        // Legacy A2A messages remain valid and execute directly against the
        // admitted ProgramVersion, without pretending to have a link grant.
    }

    const std::string_view agent_id =
        mailbox_ ? std::string_view(mailbox_->agent_id()) : std::string_view("a2a-adapter");
    auto invocation = invocation_builder_
                          ? invocation_builder_(inbound, task_id, context_id)
                          : default_invocation(
                                version_, owner_scope_, agent_id, envelope, inbound, task_id, context_id);
    try {
        invocation.validate();
    } catch (const std::invalid_argument& error) {
        throw ProgramA2ARequestError(
            std::string("Program RunInvocation is invalid: ") + error.what());
    }
    if (invocation.owner_scope != owner_scope_ || invocation.program_version_id != version_.id() ||
        invocation.run_id != task_id || !invocation.parent_run_id.empty() ||
        (mailbox_ && invocation.agent_id != mailbox_->agent_id())) {
        throw ProgramA2ARequestError(
            "Program RunInvocation is outside the Program adapter identity boundary");
    }

    if (envelope) {
        if (!mailbox_) {
            throw ProgramA2ARequestError("Collaboration request requires a mailbox");
        }
        if (envelope->a2a_task_id != task_id || envelope->a2a_context_id != context_id ||
            envelope->receiver_owner_scope != owner_scope_ ||
            envelope->receiver_agent_id != mailbox_->agent_id() ||
            envelope->receiver_program_run_id != task_id ||
            (!envelope->program_version_id.empty() &&
             envelope->program_version_id != version_.id())) {
            throw ProgramA2ARequestError("Collaboration request identity is outside the Program boundary");
        }
        const auto idempotency_key = envelope->idempotency_key;
        const auto result = mailbox_->submit_program(std::move(*envelope), version_, invocation);
        if (result == CollaborationSubmitResult::Conflict ||
            result == CollaborationSubmitResult::Rejected) {
            throw ProgramA2ARequestError(
                "Collaboration request was rejected by owner/link/capability/effect policy");
        }
        if (result == CollaborationSubmitResult::Duplicate) {
            const auto record = mailbox_->get(idempotency_key);
            if (!record) {
                throw ProgramA2ARequestError(
                    "Collaboration idempotency record disappeared after duplicate admission");
            }
            return recover_record(*record);
        }
    } else {
        // A legacy message/send retry has no collaboration idempotency key.
        // Reconnect the exact durable run before attempting a new admission;
        // a missing run is the only case that falls through to start().
        try {
            return reconnect(task_id);
        } catch (const program::ProgramDiagnosticError& error) {
            if (error.diagnostic().code != "P_RUN_NOT_FOUND") throw;
        }
    }
    auto handle = runtime_->start(std::move(invocation));
    if (handle.run_id() != task_id) {
        throw std::runtime_error("ProgramRuntime changed the requested A2A run ID");
    }
    return handle;
}
program::ProgramHandle
ProgramAgentAdapter::recover_record(const CollaborationRecord& record) const {
    if (!record.program_request || !record.program_request->version ||
        !record.program_request->invocation) {
        throw ProgramA2ARequestError("Collaboration record lost its typed Program request");
    }

    const auto& request    = *record.program_request;
    const auto& invocation = *request.invocation;
    const auto& envelope   = record.envelope;
    const auto& run_id     = invocation.run_id;
    if (!mailbox_ || envelope.program_version_id != version_.id() ||
        request.version->id() != version_.id() ||
        request.version->serialize_canonical() != version_.serialize_canonical() ||
        invocation.owner_scope != owner_scope_ || invocation.agent_id != mailbox_->agent_id() ||
        invocation.program_version_id != version_.id() || run_id.empty() ||
        envelope.a2a_task_id != run_id || envelope.receiver_program_run_id != run_id ||
        envelope.a2a_context_id.empty() || envelope.receiver_owner_scope != owner_scope_ ||
        !invocation.parent_run_id.empty() || invocation.message_sequence != envelope.sequence ||
        invocation.idempotency_key != envelope.idempotency_key ||
        invocation.correlation_id != envelope.correlation_id ||
        invocation.input != envelope.payload) {
        throw ProgramA2ARequestError(
            "Collaboration record is outside the Program adapter identity boundary");
    }

    const auto verify_handle = [&](program::ProgramHandle handle) {
        const auto durable = handle.snapshot();
        if (handle.run_id() != run_id || handle.program_version_id() != version_.id() ||
            durable.program_version_id() != version_.id() || durable.invocation() != invocation) {
            throw ProgramA2ARequestError(
                "Durable Program run does not match the accepted collaboration request");
        }
        return handle;
    };

    try {
        return verify_handle(reconnect(run_id));
    } catch (const program::ProgramDiagnosticError& error) {
        if (error.diagnostic().code != "P_RUN_NOT_FOUND") throw;
    }

    if (record.state != CollaborationRecordState::Accepted) {
        throw ProgramA2ARequestError(
            "Acknowledged/canceled collaboration record has no durable Program run");
    }

    try {
        return verify_handle(runtime_->start(invocation));
    } catch (const program::ProgramDiagnosticError& error) {
        if (error.diagnostic().code != "P_RUN_CONFLICT") throw;
        return verify_handle(reconnect(run_id));
    }
}


std::vector<program::ProgramHandle> ProgramAgentAdapter::recover_pending() const {
    std::vector<program::ProgramHandle> recovered;
    if (!mailbox_) return recovered;

    const auto records = mailbox_->snapshot();
    const auto adapter_version = version_.serialize_canonical();
    std::vector<const CollaborationRecord*> pending;
    std::unordered_set<std::string>          recovered_run_ids;
    pending.reserve(records.size());

    // Validate the complete recovery batch before any request is dispatched:
    // a later duplicate must not make an earlier record observable as a
    // partially accepted Program run.
    for (const auto& record : records) {
        if (record.state != CollaborationRecordState::Accepted) continue;
        if (record.envelope.program_version_id != version_.id()) continue;
        if (!record.program_request) {
            throw ProgramA2ARequestError("Accepted collaboration record lost its typed Program request");
        }

        const auto& request = *record.program_request;
        if (!request.version || !request.invocation ||
            request.version->id() != version_.id() ||
            request.version->serialize_canonical() != adapter_version) {
            throw ProgramA2ARequestError(
                "Accepted collaboration record does not preserve the adapter ProgramVersion");
        }

        const auto& invocation = *request.invocation;
        const auto& envelope   = record.envelope;
        const auto& run_id     = invocation.run_id;
        if (run_id.empty() || invocation.owner_scope != owner_scope_ ||
            invocation.agent_id != mailbox_->agent_id() ||
            invocation.program_version_id != version_.id() ||
            envelope.a2a_task_id != run_id || envelope.receiver_program_run_id != run_id ||
            envelope.a2a_context_id.empty() || envelope.receiver_owner_scope != owner_scope_ ||
            !invocation.parent_run_id.empty() ||
            invocation.message_sequence != envelope.sequence ||
            invocation.idempotency_key != envelope.idempotency_key ||
            invocation.correlation_id != envelope.correlation_id ||
            invocation.input != envelope.payload) {
            throw ProgramA2ARequestError(
                "Accepted collaboration record is outside the Program adapter identity boundary");
        }
        // Revalidate the durable request against the receiver's current link
        // before dispatch.  A link may have been revoked after the mailbox
        // snapshot was written (or while an older journal is being reopened);
        // submit_program returns Duplicate only when the exact request still
        // passes the live owner/capability/expiry checks.
        if (mailbox_->submit_program(envelope, *request.version, *request.invocation) !=
            CollaborationSubmitResult::Duplicate) {
            continue;
        }
        if (!recovered_run_ids.insert(run_id).second) {
            throw ProgramA2ARequestError(
                "Multiple accepted collaboration records claim the same Program run ID");
        }
        pending.push_back(&record);
    }

    for (const auto* record : pending) {
        recovered.push_back(recover_record(*record));
    }
    return recovered;
}

program::ProgramHandle ProgramAgentAdapter::reconnect(std::string_view task_id) const {
    auto handle = runtime_->reconnect(owner_scope_, task_id);
    if (handle.program_version_id() != version_.id()) {
        throw ProgramA2ARequestError("Program run version does not match A2A adapter version");
    }
    return handle;
}

Task ProgramAgentAdapter::task_snapshot(const program::ProgramHandle& handle,
                                        std::string_view task_id,
                                        std::string_view context_id) const {
    Task task;
    task.id = std::string(task_id);
    task.context_id = std::string(context_id.empty() ? task_id : context_id);
    task.metadata = json{{"program_run_id", handle.run_id()},
                         {"program_version_id", handle.program_version_id()},
                         {"reconnect_safe", true}};
    const auto record = handle.snapshot();
    if (const auto result = handle.try_result()) {
        task.status.state = task_state(result->status());
        auto message = result_message(*result, task.id, task.context_id);
        task.status.message = message;
        task.history.push_back(message);
        if (result->status() == program::ProgramTerminalStatus::Completed ||
            result->status() == program::ProgramTerminalStatus::Interrupted) {
            auto artifact = artifact_builder_ ? artifact_builder_(*result, task.id)
                                              : std::optional<Artifact>{default_artifact(*result, task.id)};
            if (artifact) {
                // A collaboration result remains subject to the receiver's
                // artifact allowlist. Legacy A2A requests have no link and
                // therefore retain the pre-adapter publication behavior.
                bool permitted = true;
                bool linked_request = false;
                if (mailbox_) {
                    for (const auto& request : mailbox_->snapshot()) {
                        if (request.envelope.receiver_program_run_id != task.id &&
                            request.envelope.a2a_task_id != task.id) {
                            continue;
                        }
                        linked_request = true;
                        permitted = mailbox_->permits_artifact(request.envelope.link_id,
                                                               artifact->artifact_id);
                        break;
                    }
                }
                if (!linked_request || permitted) {
                    task.artifacts.push_back(std::move(*artifact));
                } else {
                    task.metadata["artifact_suppressed"] = "link_not_granted";
                }
            }
        }
        task.metadata["program_result_id"] = result->id();
        task.metadata["program_status"] = std::string(program::to_string(result->status()));
        return task;
    }
    task.status.state = continuation_state(record.continuation().state);
    if (task.status.state == TaskState::InputRequired) {
        Message message;
        message.message_id = "program-input-required:" + std::string(task_id);
        message.role = Role::Agent;
        message.task_id = std::string(task_id);
        message.context_id = task.context_id;
        message.parts.push_back(Part::text_part("Program input is required"));
        task.status.message = std::move(message);
    }
    return task;
}

Task ProgramAgentAdapter::reconnect_task(std::string_view task_id,
                                         std::string_view context_id) const {
    auto handle = reconnect(task_id);
    std::string resolved_context(context_id);
    if (resolved_context.empty() && mailbox_) {
        for (const auto& record : mailbox_->snapshot()) {
            if (record.envelope.receiver_program_run_id == task_id ||
                record.envelope.a2a_task_id == task_id) {
                resolved_context = record.envelope.a2a_context_id;
                break;
            }
        }
    }
    return task_snapshot(handle, task_id,
                         resolved_context.empty() ? std::string(task_id) : resolved_context);
}

void ProgramAgentAdapter::acknowledge_task(std::string_view task_id) const {
    if (!mailbox_) return;
    for (const auto& record : mailbox_->snapshot()) {
        if (record.envelope.receiver_program_run_id == task_id ||
            record.envelope.a2a_task_id == task_id) {
            mailbox_->acknowledge(record.envelope.idempotency_key);
        }
    }
}

}  // namespace neograph::a2a
