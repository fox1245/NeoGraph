#include <neograph/program/runtime_instruction.h>

#include "canonical_json.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace neograph::program {
namespace {

void token(std::string_view value, std::string_view field) {
    if (value.empty() || value.size() > 4096 ||
        value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(std::string(field) + " is invalid");
    }
}

void sha(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void normalize_tokens(std::vector<std::string>& values, std::string_view field) {
    for (const auto& value : values) token(value, field);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    }
}

std::string action_name(RuntimeInstructionAction action) {
    return std::string(to_string(action));
}

std::string required_string(const json& value, std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Stored runtime instruction field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::vector<std::string> string_array(const json& value, std::string_view name) {
    const std::string key(name);
    if (!value.contains(key) || !value.at(key).is_array()) {
        throw std::invalid_argument("Stored runtime instruction field '" + key +
                                    "' must be an array");
    }
    std::vector<std::string> result;
    for (const auto& item : value.at(key)) {
        if (!item.is_string()) {
            throw std::invalid_argument("Stored runtime instruction array is invalid");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

json instruction_body(const RuntimeDeveloperInstructionData& data) {
    return {{"format", "neograph-runtime-developer-instruction"},
            {"storage_schema_version", 1},
            {"owner_scope", data.owner_scope},
            {"source_run_id", data.source_run_id},
            {"feed_id", data.feed_id},
            {"sequence", data.sequence},
            {"predecessor_record_id", data.predecessor_record_id
                                           ? json(*data.predecessor_record_id)
                                           : json(nullptr)},
            {"submitted_at_ms", data.submitted_at_ms},
            {"text", data.text},
            {"payload", data.payload},
            {"requested_capabilities", data.requested_capabilities},
            {"requested_effects", data.requested_effects}};
}

json decision_body(const RuntimeInstructionDecisionData& data) {
    return {{"format", "neograph-runtime-instruction-decision"},
            {"storage_schema_version", 1},
            {"instruction_id", data.instruction_id},
            {"source_run_id", data.source_run_id},
            {"expected_lineage_head_id", data.expected_lineage_head_id},
            {"policy_identity", data.policy_identity},
            {"action", action_name(data.action)},
            {"target_program_version_id", data.target_program_version_id},
            {"target_run_id", data.target_run_id},
            {"reason", data.reason}};
}

}  // namespace

struct RuntimeDeveloperInstruction::Impl {
    RuntimeDeveloperInstructionData data;
    std::string id;
    std::string canonical;
};

RuntimeDeveloperInstruction::RuntimeDeveloperInstruction(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

RuntimeDeveloperInstruction RuntimeDeveloperInstruction::create(
    RuntimeDeveloperInstructionData data) {
    token(data.owner_scope, "Runtime instruction owner_scope");
    token(data.source_run_id, "Runtime instruction source_run_id");
    token(data.feed_id, "Runtime instruction feed_id");
    if (!data.sequence || data.submitted_at_ms < 0 || data.text.empty() ||
        data.text.size() > 1024 * 1024) {
        throw std::invalid_argument("Runtime instruction sequence, time, or text is invalid");
    }
    if (data.predecessor_record_id) {
        sha(*data.predecessor_record_id,
            "Runtime instruction predecessor_record_id");
    }
    normalize_tokens(data.requested_capabilities,
                     "Runtime instruction requested_capabilities");
    normalize_tokens(data.requested_effects,
                     "Runtime instruction requested_effects");
    data.payload = detail::owned_json_copy(data.payload);
    auto body = instruction_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity(
        "runtime-developer-instruction/v1", detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return RuntimeDeveloperInstruction(std::move(impl));
}

RuntimeDeveloperInstruction RuntimeDeveloperInstruction::parse(
    std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    detail::reject_unknown_fields(
        value, "Stored RuntimeDeveloperInstruction",
        {"format", "storage_schema_version", "id", "owner_scope",
         "source_run_id", "feed_id", "sequence", "predecessor_record_id",
         "submitted_at_ms", "text", "payload", "requested_capabilities",
         "requested_effects"});
    if (!value.is_object() ||
        required_string(value, "format") !=
            "neograph-runtime-developer-instruction" ||
        !value.at("storage_schema_version").is_number_unsigned() ||
        value.at("storage_schema_version").get<std::uint32_t>() !=
            STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored RuntimeDeveloperInstruction is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    RuntimeDeveloperInstructionData data;
    data.owner_scope = required_string(value, "owner_scope");
    data.source_run_id = required_string(value, "source_run_id");
    data.feed_id = required_string(value, "feed_id");
    data.sequence = value.at("sequence").get<std::uint64_t>();
    if (!value.at("predecessor_record_id").is_null()) {
        data.predecessor_record_id =
            value.at("predecessor_record_id").get<std::string>();
    }
    data.submitted_at_ms = value.at("submitted_at_ms").get<std::int64_t>();
    data.text = required_string(value, "text");
    data.payload = value.at("payload");
    data.requested_capabilities = string_array(value, "requested_capabilities");
    data.requested_effects = string_array(value, "requested_effects");
    auto result = create(std::move(data));
    if (result.id() != stored_id) {
        throw std::invalid_argument("Stored RuntimeDeveloperInstruction id mismatch");
    }
    return result;
}

const RuntimeDeveloperInstructionData& RuntimeDeveloperInstruction::data() const noexcept {
    return impl_->data;
}
const std::string& RuntimeDeveloperInstruction::id() const noexcept { return impl_->id; }
std::string RuntimeDeveloperInstruction::serialize_canonical() const {
    return impl_->canonical;
}

std::string_view to_string(RuntimeInstructionAction action) noexcept {
    switch (action) {
        case RuntimeInstructionAction::SatisfiedInPlace: return "satisfied_in_place";
        case RuntimeInstructionAction::Rejected: return "rejected";
        case RuntimeInstructionAction::ReplaceAtHandoff: return "replace_at_handoff";
        case RuntimeInstructionAction::MigrateGraph: return "migrate_graph";
    }
    return "unknown";
}

RuntimeInstructionAction runtime_instruction_action_from_string(
    std::string_view value) {
    if (value == "satisfied_in_place") return RuntimeInstructionAction::SatisfiedInPlace;
    if (value == "rejected") return RuntimeInstructionAction::Rejected;
    if (value == "replace_at_handoff") return RuntimeInstructionAction::ReplaceAtHandoff;
    if (value == "migrate_graph") return RuntimeInstructionAction::MigrateGraph;
    throw std::invalid_argument("Runtime instruction action is unsupported");
}

struct RuntimeInstructionDecision::Impl {
    RuntimeInstructionDecisionData data;
    std::string id;
    std::string canonical;
};

RuntimeInstructionDecision::RuntimeInstructionDecision(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

RuntimeInstructionDecision RuntimeInstructionDecision::create(
    RuntimeInstructionDecisionData data) {
    sha(data.instruction_id, "Runtime instruction decision instruction_id");
    token(data.source_run_id, "Runtime instruction decision source_run_id");
    sha(data.expected_lineage_head_id,
        "Runtime instruction decision expected_lineage_head_id");
    sha(data.policy_identity, "Runtime instruction decision policy_identity");
    if (to_string(data.action) == "unknown" || data.reason.empty() ||
        data.reason.size() > 4096) {
        throw std::invalid_argument("Runtime instruction decision is invalid");
    }
    const bool transition =
        data.action == RuntimeInstructionAction::ReplaceAtHandoff ||
        data.action == RuntimeInstructionAction::MigrateGraph;
    if (transition) {
        sha(data.target_program_version_id,
            "Runtime instruction decision target_program_version_id");
        token(data.target_run_id, "Runtime instruction decision target_run_id");
    } else if (!data.target_program_version_id.empty() || !data.target_run_id.empty()) {
        throw std::invalid_argument(
            "Non-transition runtime instruction decision cannot name a target");
    }
    auto body = decision_body(data);
    auto impl = std::make_shared<Impl>();
    impl->id = detail::sha256_identity(
        "runtime-instruction-decision/v1", detail::canonical_json_bytes(body));
    body["id"] = impl->id;
    impl->data = std::move(data);
    impl->canonical = detail::canonical_json_bytes(body);
    return RuntimeInstructionDecision(std::move(impl));
}

RuntimeInstructionDecision RuntimeInstructionDecision::parse(
    std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    detail::reject_unknown_fields(
        value, "Stored RuntimeInstructionDecision",
        {"format", "storage_schema_version", "id", "instruction_id",
         "source_run_id", "expected_lineage_head_id", "policy_identity",
         "action", "target_program_version_id", "target_run_id", "reason"});
    if (!value.is_object() ||
        required_string(value, "format") !=
            "neograph-runtime-instruction-decision" ||
        !value.at("storage_schema_version").is_number_unsigned() ||
        value.at("storage_schema_version").get<std::uint32_t>() !=
            STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored RuntimeInstructionDecision is unsupported");
    }
    const auto stored_id = required_string(value, "id");
    RuntimeInstructionDecisionData data;
    data.instruction_id = required_string(value, "instruction_id");
    data.source_run_id = required_string(value, "source_run_id");
    data.expected_lineage_head_id =
        required_string(value, "expected_lineage_head_id");
    data.policy_identity = required_string(value, "policy_identity");
    data.action = runtime_instruction_action_from_string(required_string(value, "action"));
    data.target_program_version_id =
        required_string(value, "target_program_version_id");
    data.target_run_id = required_string(value, "target_run_id");
    data.reason = required_string(value, "reason");
    auto result = create(std::move(data));
    if (result.id() != stored_id) {
        throw std::invalid_argument("Stored RuntimeInstructionDecision id mismatch");
    }
    return result;
}

const RuntimeInstructionDecisionData& RuntimeInstructionDecision::data() const noexcept {
    return impl_->data;
}
const std::string& RuntimeInstructionDecision::id() const noexcept { return impl_->id; }
std::string RuntimeInstructionDecision::serialize_canonical() const {
    return impl_->canonical;
}

RuntimeInstructionController::RuntimeInstructionController(
    RuntimeInstructionControllerConfig config)
    : config_(std::move(config)) {
    if (!config_.context_store || !config_.transitions || !config_.catalog ||
        !config_.planner) {
        throw std::invalid_argument(
            "Runtime instruction controller requires stores, Catalog, and planner");
    }
}

RuntimeInstructionPlan RuntimeInstructionController::submit_and_plan(
    const RuntimeDeveloperInstruction& instruction,
    const std::optional<std::string>& expected_history_head_id) {
    const auto& request = instruction.data();
    RuntimeHistoryRecordData history;
    history.feed_id = request.feed_id;
    history.sequence = request.sequence;
    history.message_id = instruction.id();
    history.trust = RuntimeTrustClass::Developer;
    history.message = {"system", request.text};
    history.source_payload = detail::parse_json_strict(instruction.serialize_canonical());
    history.source_media_type =
        "application/vnd.neograph.runtime-developer-instruction+json";
    history.predecessor_id = request.predecessor_record_id;
    auto record = RuntimeHistoryRecord::create(std::move(history));
    const auto appended = config_.context_store->append_history(
        {request.owner_scope, request.feed_id}, record, expected_history_head_id);
    if (appended == ContextStoreAppendResult::Conflict) {
        throw std::runtime_error("Runtime developer instruction history append conflicted");
    }

    const auto lineage = config_.transitions->load_run_lineage(
        request.owner_scope, request.source_run_id);
    if (!lineage || lineage->active_run_record_id().empty()) {
        throw std::runtime_error("Runtime developer instruction source lineage is unavailable");
    }
    const auto generation = config_.transitions->load_generation(
        request.owner_scope, lineage->lineage_id(), lineage->active_generation());
    if (!generation || generation->run_id() != request.source_run_id ||
        generation->id() != lineage->active_generation_id()) {
        throw std::runtime_error(
            "Runtime developer instruction does not target the active generation");
    }
    auto decision_data = config_.planner(instruction, *lineage, *generation);
    if (decision_data.instruction_id.empty()) decision_data.instruction_id = instruction.id();
    if (decision_data.source_run_id.empty()) decision_data.source_run_id = request.source_run_id;
    if (decision_data.expected_lineage_head_id.empty()) {
        decision_data.expected_lineage_head_id = lineage->id();
    }
    auto decision = RuntimeInstructionDecision::create(std::move(decision_data));
    if (decision.data().instruction_id != instruction.id() ||
        decision.data().source_run_id != request.source_run_id ||
        decision.data().expected_lineage_head_id != lineage->id()) {
        throw std::runtime_error(
            "Runtime instruction planner decision does not bind the active source");
    }
    const bool transition =
        decision.data().action == RuntimeInstructionAction::ReplaceAtHandoff ||
        decision.data().action == RuntimeInstructionAction::MigrateGraph;
    if (transition && !config_.catalog->find_version(
                          request.owner_scope,
                          decision.data().target_program_version_id)) {
        throw std::runtime_error(
            "Runtime instruction target is not an exact admitted ProgramVersion");
    }

    ContextArtifactData artifact;
    artifact.kind = ContextArtifactKind::DerivedContext;
    artifact.producer_id = "runtime-instruction-controller.v1";
    artifact.source_digest = instruction.id();
    artifact.source_feed_id = request.feed_id;
    artifact.covers_from_sequence = request.sequence;
    artifact.covers_through_sequence = request.sequence;
    artifact.media_type = "text/plain";
    artifact.required = true;
    artifact.content = decision.serialize_canonical();
    auto decision_artifact = ContextArtifact::create(std::move(artifact));
    const auto stored = config_.context_store->put_artifact(
        request.owner_scope, decision_artifact);
    if (stored == ContextArtifactPutResult::Conflict) {
        throw std::runtime_error("Runtime instruction decision artifact conflicted");
    }
    return {std::move(record), std::move(decision),
            std::move(decision_artifact)};
}

ProgramHandle RuntimeInstructionController::apply_replacement(
    const RuntimeInstructionDecision& decision,
    ProgramRuntime& runtime, ExactProgramHandoffReference source,
    RunInvocation invocation,
    std::shared_ptr<ProgramEventSink> events) const {
    if (decision.data().action != RuntimeInstructionAction::ReplaceAtHandoff ||
        source.source_run_id != decision.data().source_run_id ||
        invocation.owner_scope.empty() ||
        invocation.program_version_id != decision.data().target_program_version_id ||
        invocation.run_id != decision.data().target_run_id ||
        invocation.correlation_id != decision.data().instruction_id) {
        throw std::invalid_argument(
            "Runtime instruction replacement request does not bind its decision");
    }
    const auto lineage = config_.transitions->load_run_lineage(
        invocation.owner_scope, decision.data().source_run_id);
    if (!lineage || lineage->id() != decision.data().expected_lineage_head_id) {
        throw std::runtime_error(
            "Runtime instruction replacement decision is stale");
    }
    return runtime.replace(std::move(source), std::move(invocation),
                           std::move(events));
}

ProgramHandle RuntimeInstructionController::apply_graph_migration(
    const RuntimeInstructionDecision& decision, ProgramRuntime& runtime,
    const ProgramHandle& source,
    std::shared_ptr<ProgramEventSink> events) const {
    if (decision.data().action != RuntimeInstructionAction::MigrateGraph ||
        source.run_id() != decision.data().source_run_id) {
        throw std::invalid_argument(
            "Runtime instruction graph migration does not bind its decision");
    }
    const auto snapshot = source.snapshot();
    const auto lineage = config_.transitions->load_run_lineage(
        snapshot.owner_scope(), decision.data().source_run_id);
    if (!lineage || lineage->id() != decision.data().expected_lineage_head_id) {
        throw std::runtime_error(
            "Runtime instruction graph migration decision is stale");
    }
    return runtime.migrate_graph(
        source,
        ProgramGraphMigrationTarget{decision.data().target_program_version_id,
                                    decision.data().target_run_id,
                                    std::move(events)});
}

}  // namespace neograph::program
