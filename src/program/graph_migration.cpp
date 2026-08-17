#include <neograph/program/graph_migration.h>

#include <neograph/program/lineage.h>
#include <neograph/program/migration.h>
#include <neograph/program/event.h>
#include <neograph/program/replacement.h>
#include <neograph/program/run_record.h>
#include <neograph/program/transition_store.h>
#include <neograph/program/version.h>

#include "canonical_json.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view CAPSULE_FORMAT = "neograph-graph-migration-capsule";
constexpr std::string_view RECEIPT_FORMAT = "neograph-program-graph-migration-receipt";
constexpr std::string_view EXECUTION_LEASE_FORMAT = "neograph-program-execution-lease";

struct CapsuleData {
    std::string            owner_scope;
    std::string            lineage_id;
    std::uint64_t          source_generation = 0;
    std::string            source_generation_id;
    std::string            source_lineage_head_id;
    std::string            source_run_id;
    std::string            source_program_version_id;
    std::string            source_bundle_id;
    CoreCheckpointIdentity core_checkpoint;
    graph::Checkpoint      checkpoint;
};

std::string require_string(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_string()) {
        throw std::invalid_argument("Graph migration capsule field '" + key +
                                    "' must be a string");
    }
    return value.at(key).get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_unsigned()) {
        throw std::invalid_argument("Graph migration capsule field '" + key +
                                    "' must be unsigned");
    }
    return value.at(key).get<std::uint64_t>();
}

std::uint32_t require_uint32(const json& value, std::string_view field) {
    const auto result = require_uint64(value, field);
    if (result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Graph migration capsule integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(result);
}

std::int64_t require_int64(const json& value, std::string_view field) {
    const auto key = std::string(field);
    if (!value.contains(key) || !value.at(key).is_number_integer()) {
        throw std::invalid_argument("Graph migration capsule field '" + key +
                                    "' must be an integer");
    }
    if (value.at(key).is_number_unsigned()) {
        const auto result = value.at(key).get<std::uint64_t>();
        if (result > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw std::invalid_argument("Graph migration capsule integer exceeds int64 range");
        }
        return static_cast<std::int64_t>(result);
    }
    return value.at(key).get<std::int64_t>();
}

json encode_core_checkpoint(const CoreCheckpointIdentity& checkpoint) {
    return {{"core_name", checkpoint.core_name},
            {"core_generation_id", checkpoint.core_generation_id},
            {"core_thread_id", checkpoint.core_thread_id},
            {"checkpoint_id", checkpoint.checkpoint_id},
            {"checkpoint_schema_version", checkpoint.checkpoint_schema_version}};
}

CoreCheckpointIdentity parse_core_checkpoint(const json& value) {
    detail::reject_unknown_fields(
        value, "Graph migration capsule Core checkpoint",
        {"core_name", "core_generation_id", "core_thread_id", "checkpoint_id",
         "checkpoint_schema_version"});
    return {require_string(value, "core_name"),
            require_string(value, "core_generation_id"),
            require_string(value, "core_thread_id"),
            require_string(value, "checkpoint_id"),
            require_uint32(value, "checkpoint_schema_version")};
}

json encode_barriers(const graph::Checkpoint& checkpoint) {
    json barriers = json::object();
    for (const auto& [barrier, signals] : checkpoint.barrier_state) {
        json encoded = json::array();
        for (const auto& signal : signals) encoded.push_back(signal);
        barriers[barrier] = std::move(encoded);
    }
    return barriers;
}

json encode_checkpoint(const graph::Checkpoint& checkpoint) {
    return {{"id", checkpoint.id},
            {"thread_id", checkpoint.thread_id},
            {"channel_values", checkpoint.channel_values},
            {"channel_versions", checkpoint.channel_versions},
            {"parent_id", checkpoint.parent_id},
            {"current_node", checkpoint.current_node},
            {"next_nodes", checkpoint.next_nodes},
            {"phase", std::string(graph::to_string(checkpoint.interrupt_phase))},
            {"barrier_state", encode_barriers(checkpoint)},
            {"metadata", checkpoint.metadata},
            {"step", checkpoint.step},
            {"timestamp", checkpoint.timestamp},
            {"schema_version", checkpoint.schema_version}};
}

graph::Checkpoint parse_checkpoint(const json& value) {
    detail::reject_unknown_fields(
        value, "Graph migration capsule checkpoint",
        {"id", "thread_id", "channel_values", "channel_versions", "parent_id",
         "current_node", "next_nodes", "phase", "barrier_state", "metadata", "step",
         "timestamp", "schema_version"});

    graph::Checkpoint checkpoint;
    checkpoint.id               = require_string(value, "id");
    checkpoint.thread_id        = require_string(value, "thread_id");
    if (!value.contains("channel_values") || !value.contains("channel_versions") ||
        !value.contains("barrier_state") || !value.contains("metadata")) {
        throw std::invalid_argument("Graph migration capsule checkpoint is incomplete");
    }
    checkpoint.channel_values   = value.at("channel_values");
    checkpoint.channel_versions = value.at("channel_versions");
    checkpoint.parent_id        = require_string(value, "parent_id");
    checkpoint.current_node     = require_string(value, "current_node");
    checkpoint.interrupt_phase  = graph::parse_checkpoint_phase(require_string(value, "phase"));
    checkpoint.metadata         = value.at("metadata");
    checkpoint.step             = require_int64(value, "step");
    checkpoint.timestamp        = require_int64(value, "timestamp");
    checkpoint.schema_version   = require_uint32(value, "schema_version");

    if (!value.contains("next_nodes") || !value.at("next_nodes").is_array()) {
        throw std::invalid_argument("Graph migration capsule next_nodes must be an array");
    }
    for (const auto& node : value.at("next_nodes")) {
        if (!node.is_string()) {
            throw std::invalid_argument(
                "Graph migration capsule next_nodes entries must be strings");
        }
        checkpoint.next_nodes.push_back(node.get<std::string>());
    }

    const auto barriers = value.at("barrier_state");
    if (!barriers.is_object()) {
        throw std::invalid_argument("Graph migration capsule barrier_state must be an object");
    }
    for (const auto& [barrier, signals] : barriers.items()) {
        if (!signals.is_array()) {
            throw std::invalid_argument(
                "Graph migration capsule barrier signals must be an array");
        }
        auto& parsed_signals = checkpoint.barrier_state[barrier];
        for (const auto& signal : signals) {
            if (!signal.is_string()) {
                throw std::invalid_argument(
                    "Graph migration capsule barrier signals must be strings");
            }
            if (!parsed_signals.insert(signal.get<std::string>()).second) {
                throw std::invalid_argument(
                    "Graph migration capsule barrier signals must be unique");
            }
        }
    }
    return checkpoint;
}

void require_identity(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256 identity");
    }
}

void validate(const CapsuleData& data) {
    detail::validate_token(data.owner_scope, "Graph migration capsule owner scope");
    detail::validate_token(data.source_run_id, "Graph migration capsule source run id");
    detail::validate_token(data.core_checkpoint.core_name,
                           "Graph migration capsule Core name");
    detail::validate_token(data.core_checkpoint.core_thread_id,
                           "Graph migration capsule Core thread id");
    detail::validate_token(data.core_checkpoint.checkpoint_id,
                           "Graph migration capsule checkpoint id");
    require_identity(data.lineage_id, "Graph migration capsule lineage id");
    require_identity(data.source_generation_id,
                     "Graph migration capsule source generation id");
    require_identity(data.source_lineage_head_id,
                     "Graph migration capsule source lineage head id");
    require_identity(data.source_program_version_id,
                     "Graph migration capsule ProgramVersion id");
    require_identity(data.source_bundle_id, "Graph migration capsule bundle id");
    require_identity(data.core_checkpoint.core_generation_id,
                     "Graph migration capsule Core generation id");
    if (data.source_generation == 0) {
        throw std::invalid_argument("Graph migration capsule source generation must be positive");
    }

    const auto& checkpoint = data.checkpoint;
    if (checkpoint.id != data.core_checkpoint.checkpoint_id ||
        checkpoint.thread_id != data.core_checkpoint.core_thread_id ||
        checkpoint.schema_version != data.core_checkpoint.checkpoint_schema_version ||
        checkpoint.schema_version != graph::CHECKPOINT_SCHEMA_VERSION) {
        throw std::invalid_argument(
            "Graph migration capsule checkpoint identity does not match its snapshot");
    }
    if (data.core_checkpoint.core_thread_id !=
        program_root_core_thread_id(data.source_run_id,
                                    data.core_checkpoint.core_generation_id)) {
        throw std::invalid_argument(
            "Graph migration capsule checkpoint is not the source root Core thread");
    }
    if (checkpoint.interrupt_phase != graph::CheckpointPhase::Completed) {
        throw std::invalid_argument(
            "Graph migration capsule requires a completed super-step checkpoint");
    }
    if (!checkpoint.channel_values.is_object() ||
        !checkpoint.channel_values.contains("channels") ||
        !checkpoint.channel_values.at("channels").is_object() ||
        !checkpoint.channel_values.contains("global_version") ||
        !checkpoint.channel_values.at("global_version").is_number_unsigned()) {
        throw std::invalid_argument(
            "Graph migration capsule requires canonical serialized channel state");
    }
    detail::reject_unknown_fields(
        checkpoint.channel_values, "Graph migration capsule channel state",
        {"channels", "global_version"});
    const auto global_version =
        checkpoint.channel_values.at("global_version").get<std::uint64_t>();
    if (global_version == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument(
            "Graph migration capsule global channel version is exhausted");
    }
    for (const auto& [name, channel] :
         checkpoint.channel_values.at("channels").items()) {
        detail::validate_token(name, "Graph migration capsule channel name");
        if (!channel.is_object()) {
            throw std::invalid_argument(
                "Graph migration capsule channel entries must be objects");
        }
        detail::reject_unknown_fields(
            channel, "Graph migration capsule channel entry", {"value", "version"});
        if (!channel.contains("value") || !channel.contains("version") ||
            !channel.at("version").is_number_unsigned()) {
            throw std::invalid_argument(
                "Graph migration capsule channel entry is incomplete");
        }
        if (channel.at("version").get<std::uint64_t>() > global_version) {
            throw std::invalid_argument(
                "Graph migration capsule channel version exceeds global version");
        }
    }
    if (!checkpoint.channel_versions.is_null()) {
        throw std::invalid_argument(
            "Graph migration capsule requires canonical embedded channel versions");
    }
    if (checkpoint.next_nodes.empty() || checkpoint.step < 0 ||
        checkpoint.step >= INT_MAX - 1 || checkpoint.timestamp < 0) {
        throw std::invalid_argument(
            "Graph migration capsule checkpoint boundary is incomplete");
    }
    detail::validate_token(checkpoint.current_node,
                           "Graph migration capsule checkpoint current node");
    std::set<std::string> unique_nodes;
    for (const auto& node : checkpoint.next_nodes) {
        detail::validate_token(node, "Graph migration capsule frontier node");
        if (!unique_nodes.insert(node).second) {
            throw std::invalid_argument(
                "Graph migration capsule frontier must not contain duplicates");
        }
    }
    for (const auto& [barrier, signals] : checkpoint.barrier_state) {
        detail::validate_token(barrier, "Graph migration capsule barrier");
        for (const auto& signal : signals) {
            detail::validate_token(signal, "Graph migration capsule barrier signal");
        }
    }
}

json body(const CapsuleData& data) {
    return {{"format", std::string(CAPSULE_FORMAT)},
            {"storage_schema_version", GraphMigrationCapsule::STORAGE_SCHEMA_VERSION},
            {"owner_scope", data.owner_scope},
            {"lineage_id", data.lineage_id},
            {"source_generation", data.source_generation},
            {"source_generation_id", data.source_generation_id},
            {"source_lineage_head_id", data.source_lineage_head_id},
            {"source_run_id", data.source_run_id},
            {"source_program_version_id", data.source_program_version_id},
            {"source_bundle_id", data.source_bundle_id},
            {"core_checkpoint", encode_core_checkpoint(data.core_checkpoint)},
            {"checkpoint", encode_checkpoint(data.checkpoint)}};
}

void validate_receipt(const ProgramGraphMigrationReceiptData& data) {
    const auto& capsule = data.capsule;
    if (data.source_bundle.id() != data.source_version.bundle_id() ||
        data.target_bundle.id() != data.target_version.bundle_id() ||
        data.source_version.id() != capsule.source_program_version_id() ||
        data.source_bundle.id() != capsule.source_bundle_id() ||
        data.source_version.ownership_scope() != capsule.owner_scope() ||
        data.target_version.ownership_scope() != capsule.owner_scope() ||
        data.target_program_version_id != data.target_version.id() ||
        data.target_bundle_id != data.target_bundle.id()) {
        throw std::invalid_argument(
            "Program Graph migration admitted artifacts do not bind the capsule");
    }
    const auto expected_plan = MigrationPlan::between(
        data.source_version, data.source_bundle,
        data.target_version, data.target_bundle);
    if (!expected_plan.is_compatible() ||
        expected_plan.id() != data.migration_plan_id) {
        throw std::invalid_argument(
            "Program Graph migration plan is not the exact admitted compatibility proof");
    }
    const auto source_control = data.source_bundle.control_source();
    const auto target_control = data.target_bundle.control_source();
    const auto& source_plan = data.source_bundle.typed_orchestration_plan();
    const auto& target_plan = data.target_bundle.typed_orchestration_plan();
    if (source_control || target_control || source_plan.nodes().size() != 1 ||
        target_plan.nodes().size() != 1 ||
        source_plan.root().operation() != ProgramOperationKind::CallCore ||
        target_plan.root().operation() != ProgramOperationKind::CallCore ||
        !source_plan.root().core() || !target_plan.root().core() ||
        *source_plan.root().core() != capsule.core_checkpoint().core_name ||
        *target_plan.root().core() != data.target_core_checkpoint.core_name) {
        throw std::invalid_argument(
            "Program Graph migration supports only a native single-root CallCore plan");
    }
    detail::validate_token(data.target_run_id,
                           "Program Graph migration target run id");
    if (data.target_run_id == capsule.source_run_id()) {
        throw std::invalid_argument(
            "Program Graph migration target run must differ from its source");
    }
    if (capsule.source_generation() == std::numeric_limits<std::uint64_t>::max() ||
        data.target_generation != capsule.source_generation() + 1) {
        throw std::invalid_argument(
            "Program Graph migration generations must be contiguous");
    }
    require_identity(data.migration_plan_id,
                     "Program Graph migration plan id");
    require_identity(data.target_program_version_id,
                     "Program Graph migration target ProgramVersion id");
    require_identity(data.target_bundle_id,
                     "Program Graph migration target bundle id");
    require_identity(data.target_invocation_id,
                     "Program Graph migration target invocation id");
    require_identity(data.target_binding_fingerprint,
                     "Program Graph migration target binding fingerprint");
    require_identity(data.target_initial_run_record_id,
                     "Program Graph migration target initial run record id");
    require_identity(data.target_initial_journal_head,
                     "Program Graph migration target initial journal head");

    const auto& target = data.target_core_checkpoint;
    detail::validate_token(target.core_name,
                           "Program Graph migration target Core name");
    detail::validate_token(target.core_thread_id,
                           "Program Graph migration target Core thread id");
    detail::validate_token(target.checkpoint_id,
                           "Program Graph migration target checkpoint id");
    require_identity(target.core_generation_id,
                     "Program Graph migration target Core generation id");
    const auto target_materialization = std::find_if(
        data.target_version.core_materialization_receipt().plans.begin(),
        data.target_version.core_materialization_receipt().plans.end(),
        [&](const CorePlanIdentity& plan) {
            return plan.name == target.core_name &&
                   plan.compiled_plan_identity == target.core_generation_id;
        });
    if (target.core_thread_id !=
            program_root_core_thread_id(data.target_run_id,
                                        target.core_generation_id) ||
        target.checkpoint_id == capsule.core_checkpoint().checkpoint_id ||
        target.checkpoint_schema_version != graph::CHECKPOINT_SCHEMA_VERSION ||
        target.core_name != capsule.core_checkpoint().core_name ||
        target.core_generation_id != capsule.core_checkpoint().core_generation_id ||
        target_materialization ==
            data.target_version.core_materialization_receipt().plans.end()) {
        throw std::invalid_argument(
            "Program Graph migration target checkpoint is not an exact root clone");
    }
    const auto& snapshot = data.target_checkpoint;
    auto expected_snapshot = capsule.checkpoint();
    expected_snapshot.id = target.checkpoint_id;
    expected_snapshot.thread_id = target.core_thread_id;
    expected_snapshot.parent_id = capsule.core_checkpoint().checkpoint_id;
    expected_snapshot.metadata["migrated_from"] = capsule.id();
    expected_snapshot.timestamp = snapshot.timestamp;
    if (snapshot.id != target.checkpoint_id ||
        snapshot.thread_id != target.core_thread_id ||
        snapshot.schema_version != target.checkpoint_schema_version ||
        snapshot.timestamp <= capsule.checkpoint().timestamp ||
        encode_checkpoint(snapshot) != encode_checkpoint(expected_snapshot)) {
        throw std::invalid_argument(
            "Program Graph migration target snapshot is not the exact admitted clone");
    }
}

json receipt_body(const ProgramGraphMigrationReceiptData& data) {
    return {{"format", std::string(RECEIPT_FORMAT)},
            {"storage_schema_version",
             ProgramGraphMigrationReceipt::STORAGE_SCHEMA_VERSION},
            {"capsule", detail::parse_json_strict(
                            data.capsule.serialize_canonical())},
            {"source_bundle", detail::parse_json_strict(
                                  data.source_bundle.serialize_canonical())},
            {"source_version", detail::parse_json_strict(
                                   data.source_version.serialize_canonical())},
            {"target_bundle", detail::parse_json_strict(
                                  data.target_bundle.serialize_canonical())},
            {"target_version", detail::parse_json_strict(
                                   data.target_version.serialize_canonical())},
            {"migration_plan_id", data.migration_plan_id},
            {"target_generation", data.target_generation},
            {"target_run_id", data.target_run_id},
            {"target_program_version_id", data.target_program_version_id},
            {"target_bundle_id", data.target_bundle_id},
            {"target_invocation_id", data.target_invocation_id},
            {"target_binding_fingerprint", data.target_binding_fingerprint},
            {"target_initial_run_record_id", data.target_initial_run_record_id},
            {"target_initial_journal_head", data.target_initial_journal_head},
            {"target_core_checkpoint",
             encode_core_checkpoint(data.target_core_checkpoint)},
            {"target_checkpoint", encode_checkpoint(data.target_checkpoint)}};
}

bool budget_at_most(const RunBudget& value, const RunBudget& limit) noexcept {
    return value.wall_time_ms <= limit.wall_time_ms &&
           value.model_tokens <= limit.model_tokens &&
           value.monetary_microunits <= limit.monetary_microunits &&
           value.max_concurrency <= limit.max_concurrency &&
           value.max_program_operations <= limit.max_program_operations &&
           value.max_core_steps <= limit.max_core_steps &&
           value.max_dynamic_compiles <= limit.max_dynamic_compiles &&
           value.max_child_depth <= limit.max_child_depth &&
           value.max_total_children <= limit.max_total_children;
}

bool budget_at_most(const RunBudget& value, const BudgetLimits& limit) noexcept {
    return value.wall_time_ms <= limit.wall_time_ms &&
           value.model_tokens <= limit.model_tokens &&
           value.monetary_microunits <= limit.monetary_microunits &&
           value.max_concurrency <= limit.max_concurrency &&
           value.max_program_operations <= limit.max_program_operations &&
           value.max_core_steps <= limit.max_core_steps &&
           value.max_dynamic_compiles <= limit.max_dynamic_compiles &&
           value.max_child_depth <= limit.max_child_depth &&
           value.max_total_children <= limit.max_total_children;
}

std::optional<std::uint64_t> budget_value(const RunBudget& budget,
                                          std::string_view resource) noexcept {
    if (resource == "wall_time_ms") return budget.wall_time_ms;
    if (resource == "model_tokens") return budget.model_tokens;
    if (resource == "monetary_microunits") return budget.monetary_microunits;
    if (resource == "max_concurrency") return budget.max_concurrency;
    if (resource == "max_program_operations") return budget.max_program_operations;
    if (resource == "max_core_steps") return budget.max_core_steps;
    if (resource == "max_dynamic_compiles") return budget.max_dynamic_compiles;
    if (resource == "max_child_depth") return budget.max_child_depth;
    if (resource == "max_total_children") return budget.max_total_children;
    return std::nullopt;
}

bool budget_fits_bundle(const RunBudget& budget,
                        const ProgramBundle& bundle) noexcept {
    for (const auto& requirement : bundle.declared_budget_requirements()) {
        const auto value = budget_value(budget, requirement.resource);
        if (!value || *value > requirement.maximum) return false;
    }
    return true;
}

bool same_migration_invocation(const RunInvocation& source,
                               const RunInvocation& target) {
    return source.protocol_revision == target.protocol_revision &&
           source.owner_scope == target.owner_scope &&
           source.agent_id == target.agent_id &&
           source.parent_run_id == target.parent_run_id &&
           detail::canonical_json_bytes(source.input) ==
               detail::canonical_json_bytes(target.input) &&
           source.message_sequence == target.message_sequence &&
           source.idempotency_key == target.idempotency_key &&
           source.correlation_id == target.correlation_id &&
           source.artifact == target.artifact;
}

void validate_execution_lease(const ProgramExecutionLeaseData& data) {
    detail::validate_token(data.owner_scope, "Program execution lease owner scope");
    detail::validate_token(data.run_id, "Program execution lease run id");
    detail::validate_token(data.core_name, "Program execution lease Core name");
    detail::validate_token(data.holder_id, "Program execution lease holder id");
    require_identity(data.program_version_id, "Program execution lease ProgramVersion id");
    require_identity(data.bundle_id, "Program execution lease bundle id");
    require_identity(data.core_generation_id, "Program execution lease Core generation id");
    require_identity(data.core_thread_id, "Program execution lease Core thread id");
    if (!data.attempt || data.acquired_at_ms < 0 ||
        data.expires_at_ms <= data.acquired_at_ms ||
        data.core_thread_id !=
            program_root_core_thread_id(data.run_id, data.core_generation_id)) {
        throw std::invalid_argument("Program execution lease identity or lifetime is invalid");
    }
}

json execution_lease_body(const ProgramExecutionLeaseData& data) {
    return {{"format", std::string(EXECUTION_LEASE_FORMAT)},
            {"storage_schema_version", ProgramExecutionLease::STORAGE_SCHEMA_VERSION},
            {"owner_scope", data.owner_scope},
            {"run_id", data.run_id},
            {"attempt", data.attempt},
            {"program_version_id", data.program_version_id},
            {"bundle_id", data.bundle_id},
            {"core_name", data.core_name},
            {"core_generation_id", data.core_generation_id},
            {"core_thread_id", data.core_thread_id},
            {"holder_id", data.holder_id},
            {"acquired_at_ms", data.acquired_at_ms},
            {"expires_at_ms", data.expires_at_ms}};
}

}  // namespace

std::string program_root_core_thread_id(
    std::string_view run_id,
    std::string_view core_generation_id) {
    detail::validate_token(run_id, "Program root Core thread run id");
    require_identity(core_generation_id,
                     "Program root Core thread generation id");
    std::string identity(run_id);
    identity.push_back('\0');
    identity.append("root");
    identity.push_back('\0');
    identity.append(core_generation_id);
    return detail::sha256_identity("program-core-thread/v1", identity);
}

std::string graph_migration_checkpoint_content_id(
    const graph::Checkpoint& checkpoint) {
    return detail::sha256_identity(
        "graph-migration-checkpoint-content/v1",
        detail::canonical_json_bytes(encode_checkpoint(checkpoint)));
}

struct ProgramExecutionLease::Impl {
    explicit Impl(ProgramExecutionLeaseData value) : data(std::move(value)) {
        auto encoded = execution_lease_body(data);
        id = detail::sha256_identity(
            "program-execution-lease/v1", detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    ProgramExecutionLeaseData data;
    std::string id;
    std::string canonical_bytes;
};

ProgramExecutionLease::ProgramExecutionLease(ProgramExecutionLeaseData data) {
    validate_execution_lease(data);
    impl_ = std::make_shared<const Impl>(std::move(data));
}
ProgramExecutionLease::ProgramExecutionLease(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
ProgramExecutionLease ProgramExecutionLease::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != EXECUTION_LEASE_FORMAT) {
        throw std::invalid_argument("Stored Program execution lease has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored Program execution lease",
        {"format", "storage_schema_version", "id", "owner_scope", "run_id", "attempt",
         "program_version_id", "bundle_id", "core_name", "core_generation_id",
         "core_thread_id", "holder_id", "acquired_at_ms", "expires_at_ms"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored Program execution lease schema is unsupported");
    }
    ProgramExecutionLease lease(ProgramExecutionLeaseData{
        require_string(value, "owner_scope"), require_string(value, "run_id"),
        require_uint64(value, "attempt"), require_string(value, "program_version_id"),
        require_string(value, "bundle_id"), require_string(value, "core_name"),
        require_string(value, "core_generation_id"), require_string(value, "core_thread_id"),
        require_string(value, "holder_id"), require_int64(value, "acquired_at_ms"),
        require_int64(value, "expires_at_ms")});
    if (lease.id() != require_string(value, "id")) {
        throw std::invalid_argument("Stored Program execution lease identity is invalid");
    }
    return lease;
}

#define NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(name) \
    const std::string& ProgramExecutionLease::name() const noexcept { return impl_->data.name; }
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(owner_scope)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(run_id)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(program_version_id)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(bundle_id)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(core_name)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(core_generation_id)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(core_thread_id)
NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR(holder_id)
#undef NEOGRAPH_EXECUTION_LEASE_STRING_ACCESSOR
const std::string& ProgramExecutionLease::id() const noexcept { return impl_->id; }
std::uint64_t ProgramExecutionLease::attempt() const noexcept { return impl_->data.attempt; }
std::int64_t ProgramExecutionLease::acquired_at_ms() const noexcept {
    return impl_->data.acquired_at_ms;
}
std::int64_t ProgramExecutionLease::expires_at_ms() const noexcept {
    return impl_->data.expires_at_ms;
}
std::string ProgramExecutionLease::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool does_program_execution_lease_bind(
    const ProgramExecutionLease& lease,
    const ProgramTransitionPublication& publication) noexcept {
    try {
        const auto& run = publication.run_record;
        if (lease.owner_scope() != run.owner_scope() ||
            lease.run_id() != run.run_id() || lease.attempt() != run.continuation().attempt ||
            lease.program_version_id() != run.program_version_id() ||
            lease.bundle_id() != run.bundle_id() ||
            run.continuation().state != ContinuationState::Running ||
            lease.acquired_at_ms() != run.updated_at_ms() ||
            lease.expires_at_ms() - lease.acquired_at_ms() < 0 ||
            static_cast<std::uint64_t>(
                lease.expires_at_ms() - lease.acquired_at_ms()) !=
                run.remaining_budget().wall_time_ms) {
            return false;
        }
        if (const auto checkpoint = run.exact_checkpoint()) {
            return lease.core_name() == checkpoint->core_name &&
                   lease.core_generation_id() == checkpoint->core_generation_id &&
                   lease.core_thread_id() == checkpoint->core_thread_id;
        }
        return std::any_of(publication.events.begin(), publication.events.end(),
                           [&](const auto& event) {
                               return event.kind == ProgramEventKind::Started &&
                                      event.attempt == lease.attempt() &&
                                      event.core_generation_id ==
                                          lease.core_generation_id() &&
                                      event.core_run_id == lease.core_thread_id();
                           });
    } catch (const std::exception&) {
        return false;
    }
}

struct ProgramGraphSafePointEvidence::Impl {
    Impl(ProgramBundle bundle_value,
         ProgramVersion version_value,
         graph::GraphSafePoint safe_point_value)
        : bundle(std::move(bundle_value)),
          version(std::move(version_value)),
          safe_point(std::move(safe_point_value)) {}

    ProgramBundle bundle;
    ProgramVersion version;
    graph::GraphSafePoint safe_point;
};

ProgramGraphSafePointEvidence::ProgramGraphSafePointEvidence(
    ProgramBundle bundle,
    ProgramVersion version,
    graph::GraphSafePoint safe_point) {
    const auto& plan = bundle.typed_orchestration_plan();
    const auto& generation = safe_point.generation();
    const auto materialization = std::find_if(
        version.core_materialization_receipt().plans.begin(),
        version.core_materialization_receipt().plans.end(),
        [&](const CorePlanIdentity& candidate) {
            return candidate.name == generation.core_name &&
                   candidate.compiled_plan_identity == generation.core_generation_id;
        });
    if (bundle.id() != version.bundle_id() || bundle.control_source() ||
        plan.nodes().size() != 1 ||
        plan.root().operation() != ProgramOperationKind::CallCore ||
        !plan.root().core() || *plan.root().core() != generation.core_name ||
        materialization == version.core_materialization_receipt().plans.end()) {
        throw std::invalid_argument(
            "Program graph safe point does not bind an admitted native root Core");
    }
    impl_ = std::make_shared<const Impl>(
        std::move(bundle), std::move(version), std::move(safe_point));
}

const ProgramBundle& ProgramGraphSafePointEvidence::bundle() const noexcept {
    return impl_->bundle;
}
const ProgramVersion& ProgramGraphSafePointEvidence::version() const noexcept {
    return impl_->version;
}
const graph::GraphSafePoint& ProgramGraphSafePointEvidence::safe_point() const noexcept {
    return impl_->safe_point;
}

struct GraphMigrationCapsule::Impl {
    explicit Impl(CapsuleData value) : data(std::move(value)) {
        auto encoded = body(data);
        id = detail::sha256_identity("graph-migration-capsule/v1",
                                     detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    CapsuleData data;
    std::string id;
    std::string canonical_bytes;
};

GraphMigrationCapsule::GraphMigrationCapsule(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

GraphMigrationCapsule GraphMigrationCapsule::seal(
    const ProgramRunGeneration& generation,
    const ProgramRunLineage& lineage,
    const ProgramVersion& version,
    const graph::GraphSafePoint& safe_point) {
    if (generation.owner_scope() != lineage.owner_scope() ||
        generation.lineage_id() != lineage.lineage_id() ||
        generation.generation() != lineage.active_generation() ||
        generation.id() != lineage.active_generation_id()) {
        throw std::invalid_argument(
            "Graph migration capsule source generation is not the active lineage head");
    }
    if (version.id() != generation.program_version_id() ||
        version.bundle_id() != generation.bundle_id() ||
        version.ownership_scope() != generation.owner_scope()) {
        throw std::invalid_argument(
            "Graph migration capsule ProgramVersion does not bind its source generation");
    }
    const auto& safe_generation = safe_point.generation();
    const auto& plans = version.core_materialization_receipt().plans;
    const auto plan = std::find_if(
        plans.begin(), plans.end(), [&](const CorePlanIdentity& candidate) {
            return candidate.name == safe_generation.core_name &&
                   candidate.compiled_plan_identity ==
                       safe_generation.core_generation_id;
        });
    if (plan == plans.end()) {
        throw std::invalid_argument(
            "Graph migration capsule safe point is not from the admitted ProgramVersion");
    }
    const auto& checkpoint = safe_point.checkpoint();
    const auto expected_thread = program_root_core_thread_id(
        generation.run_id(), safe_generation.core_generation_id);
    if (checkpoint.thread_id != expected_thread) {
        throw std::invalid_argument(
            "Graph migration capsule supports only the source generation's root Core thread");
    }
    CoreCheckpointIdentity core_checkpoint{
        safe_generation.core_name,
        safe_generation.core_generation_id,
        checkpoint.thread_id,
        checkpoint.id,
        checkpoint.schema_version};
    CapsuleData data{generation.owner_scope(),
                     generation.lineage_id(),
                     generation.generation(),
                     generation.id(),
                     lineage.id(),
                     generation.run_id(),
                     generation.program_version_id(),
                     generation.bundle_id(),
                     std::move(core_checkpoint),
                     checkpoint};
    validate(data);
    return GraphMigrationCapsule(
        std::make_shared<const Impl>(std::move(data)));
}

GraphMigrationCapsule GraphMigrationCapsule::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != CAPSULE_FORMAT) {
        throw std::invalid_argument("Stored Graph migration capsule has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored Graph migration capsule",
        {"format", "storage_schema_version", "id", "owner_scope", "lineage_id",
         "source_generation", "source_generation_id", "source_lineage_head_id",
         "source_run_id", "source_program_version_id", "source_bundle_id",
         "core_checkpoint", "checkpoint"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored Graph migration capsule schema is unsupported");
    }
    if (!value.contains("core_checkpoint") || !value.at("core_checkpoint").is_object() ||
        !value.contains("checkpoint") || !value.at("checkpoint").is_object()) {
        throw std::invalid_argument("Stored Graph migration capsule evidence is incomplete");
    }
    CapsuleData data{require_string(value, "owner_scope"),
                     require_string(value, "lineage_id"),
                     require_uint64(value, "source_generation"),
                     require_string(value, "source_generation_id"),
                     require_string(value, "source_lineage_head_id"),
                     require_string(value, "source_run_id"),
                     require_string(value, "source_program_version_id"),
                     require_string(value, "source_bundle_id"),
                     parse_core_checkpoint(value.at("core_checkpoint")),
                     parse_checkpoint(value.at("checkpoint"))};
    validate(data);
    GraphMigrationCapsule result(
        std::make_shared<const Impl>(std::move(data)));
    if (result.id() != require_string(value, "id")) {
        throw std::invalid_argument(
            "Stored Graph migration capsule id does not match content");
    }
    return result;
}

const std::string& GraphMigrationCapsule::id() const noexcept { return impl_->id; }
const std::string& GraphMigrationCapsule::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& GraphMigrationCapsule::lineage_id() const noexcept {
    return impl_->data.lineage_id;
}
std::uint64_t GraphMigrationCapsule::source_generation() const noexcept {
    return impl_->data.source_generation;
}
const std::string& GraphMigrationCapsule::source_generation_id() const noexcept {
    return impl_->data.source_generation_id;
}
const std::string& GraphMigrationCapsule::source_lineage_head_id() const noexcept {
    return impl_->data.source_lineage_head_id;
}
const std::string& GraphMigrationCapsule::source_run_id() const noexcept {
    return impl_->data.source_run_id;
}
const std::string& GraphMigrationCapsule::source_program_version_id() const noexcept {
    return impl_->data.source_program_version_id;
}
const std::string& GraphMigrationCapsule::source_bundle_id() const noexcept {
    return impl_->data.source_bundle_id;
}
const CoreCheckpointIdentity& GraphMigrationCapsule::core_checkpoint() const noexcept {
    return impl_->data.core_checkpoint;
}
const graph::Checkpoint& GraphMigrationCapsule::checkpoint() const noexcept {
    return impl_->data.checkpoint;
}
std::string GraphMigrationCapsule::serialize_canonical() const {
    return impl_->canonical_bytes;
}

struct ProgramGraphMigrationReceipt::Impl {
    explicit Impl(ProgramGraphMigrationReceiptData value)
        : data(std::move(value)) {
        auto encoded = receipt_body(data);
        id = detail::sha256_identity(
            "program-graph-migration-receipt/v1",
            detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    ProgramGraphMigrationReceiptData data;
    std::string                      id;
    std::string                      canonical_bytes;
};

ProgramGraphMigrationReceipt::ProgramGraphMigrationReceipt(
    ProgramGraphMigrationReceiptData data) {
    validate_receipt(data);
    impl_ = std::make_shared<const Impl>(std::move(data));
}

ProgramGraphMigrationReceipt::ProgramGraphMigrationReceipt(
    std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ProgramGraphMigrationReceipt ProgramGraphMigrationReceipt::parse(
    std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != RECEIPT_FORMAT) {
        throw std::invalid_argument(
            "Stored Program Graph migration receipt has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored Program Graph migration receipt",
        {"format", "storage_schema_version", "id", "capsule", "source_bundle",
         "source_version", "target_bundle", "target_version",
         "migration_plan_id", "target_generation", "target_run_id",
         "target_program_version_id", "target_bundle_id",
         "target_invocation_id", "target_binding_fingerprint",
         "target_initial_run_record_id", "target_initial_journal_head",
         "target_core_checkpoint", "target_checkpoint"});
    if (require_uint32(value, "storage_schema_version") !=
        STORAGE_SCHEMA_VERSION || !value.contains("capsule") ||
        !value.at("capsule").is_object() ||
        !value.contains("source_bundle") || !value.at("source_bundle").is_object() ||
        !value.contains("source_version") || !value.at("source_version").is_object() ||
        !value.contains("target_bundle") || !value.at("target_bundle").is_object() ||
        !value.contains("target_version") || !value.at("target_version").is_object() ||
        !value.contains("target_core_checkpoint") ||
        !value.at("target_core_checkpoint").is_object() ||
        !value.contains("target_checkpoint") ||
        !value.at("target_checkpoint").is_object()) {
        throw std::invalid_argument(
            "Stored Program Graph migration receipt is unsupported or incomplete");
    }
    ProgramGraphMigrationReceipt result(ProgramGraphMigrationReceiptData{
        GraphMigrationCapsule::parse(
            detail::canonical_json_bytes(value.at("capsule"))),
        ProgramBundle::parse(
            detail::canonical_json_bytes(value.at("source_bundle"))),
        ProgramVersion::parse(
            detail::canonical_json_bytes(value.at("source_version"))),
        ProgramBundle::parse(
            detail::canonical_json_bytes(value.at("target_bundle"))),
        ProgramVersion::parse(
            detail::canonical_json_bytes(value.at("target_version"))),
        require_string(value, "migration_plan_id"),
        require_uint64(value, "target_generation"),
        require_string(value, "target_run_id"),
        require_string(value, "target_program_version_id"),
        require_string(value, "target_bundle_id"),
        require_string(value, "target_invocation_id"),
        require_string(value, "target_binding_fingerprint"),
        require_string(value, "target_initial_run_record_id"),
        require_string(value, "target_initial_journal_head"),
        parse_core_checkpoint(value.at("target_core_checkpoint")),
        parse_checkpoint(value.at("target_checkpoint"))});
    if (result.id() != require_string(value, "id")) {
        throw std::invalid_argument(
            "Stored Program Graph migration receipt id does not match content");
    }
    return result;
}

const std::string& ProgramGraphMigrationReceipt::id() const noexcept {
    return impl_->id;
}
const GraphMigrationCapsule&
ProgramGraphMigrationReceipt::capsule() const noexcept {
    return impl_->data.capsule;
}
const ProgramBundle& ProgramGraphMigrationReceipt::source_bundle() const noexcept {
    return impl_->data.source_bundle;
}
const ProgramVersion& ProgramGraphMigrationReceipt::source_version() const noexcept {
    return impl_->data.source_version;
}
const ProgramBundle& ProgramGraphMigrationReceipt::target_bundle() const noexcept {
    return impl_->data.target_bundle;
}
const ProgramVersion& ProgramGraphMigrationReceipt::target_version() const noexcept {
    return impl_->data.target_version;
}
const std::string&
ProgramGraphMigrationReceipt::migration_plan_id() const noexcept {
    return impl_->data.migration_plan_id;
}
std::uint64_t ProgramGraphMigrationReceipt::target_generation() const noexcept {
    return impl_->data.target_generation;
}

#define NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(name)                   \
    const std::string& ProgramGraphMigrationReceipt::name() const noexcept { \
        return impl_->data.name;                                         \
    }

NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_run_id)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_program_version_id)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_bundle_id)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_invocation_id)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_binding_fingerprint)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_initial_run_record_id)
NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR(target_initial_journal_head)

#undef NEOGRAPH_GRAPH_MIGRATION_STRING_ACCESSOR

const CoreCheckpointIdentity&
ProgramGraphMigrationReceipt::target_core_checkpoint() const noexcept {
    return impl_->data.target_core_checkpoint;
}
const graph::Checkpoint&
ProgramGraphMigrationReceipt::target_checkpoint() const noexcept {
    return impl_->data.target_checkpoint;
}
std::string ProgramGraphMigrationReceipt::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool is_valid_program_graph_migration_transition(
    const ProgramRunGeneration& predecessor,
    const ProgramRunLineage& previous_lineage,
    const ProgramRunRecord& source,
    const GraphMigrationCapsule& durable_capsule,
    const MigrationPlan& migration_plan,
    const ProgramRunGeneration& successor,
    const ProgramRunLineage& next_lineage,
    const ProgramRunRecord& target) noexcept {
    try {
        const auto receipt = successor.graph_migration_receipt();
        if (!receipt || successor.replacement_receipt()) return false;
        const auto& capsule = receipt->capsule();
        const auto source_checkpoint = source.exact_checkpoint();
        const auto target_checkpoint = target.exact_checkpoint();
        const auto migration_elapsed = target.created_at_ms() >= source.updated_at_ms()
            ? static_cast<std::uint64_t>(target.created_at_ms() - source.updated_at_ms())
            : std::numeric_limits<std::uint64_t>::max();
        const auto expected_budget = program_replacement_remaining_budget(
            source, previous_lineage, target.created_at_ms());
        const auto expected_plan = MigrationPlan::between(
            receipt->source_version(), receipt->source_bundle(),
            receipt->target_version(), receipt->target_bundle());
        return durable_capsule.id() == capsule.id() &&
               durable_capsule.serialize_canonical() == capsule.serialize_canonical() &&
               migration_plan.is_compatible() &&
               expected_plan.is_compatible() &&
               migration_plan.id() == expected_plan.id() &&
               migration_plan.id() == receipt->migration_plan_id() &&
               migration_plan.owner_scope() == source.owner_scope() &&
               migration_plan.source_version_id() == source.program_version_id() &&
               migration_plan.target_version_id() == target.program_version_id() &&
               capsule.owner_scope() == source.owner_scope() &&
               capsule.owner_scope() == target.owner_scope() &&
               capsule.lineage_id() == previous_lineage.lineage_id() &&
               capsule.source_generation() == predecessor.generation() &&
               capsule.source_generation_id() == predecessor.id() &&
               capsule.source_lineage_head_id() == previous_lineage.id() &&
               capsule.source_run_id() == source.run_id() &&
               capsule.source_program_version_id() == source.program_version_id() &&
               capsule.source_bundle_id() == source.bundle_id() && source_checkpoint &&
               *source_checkpoint == capsule.core_checkpoint() &&
               source.exact_checkpoint_content_id() &&
               *source.exact_checkpoint_content_id() ==
                   graph_migration_checkpoint_content_id(capsule.checkpoint()) &&
               receipt->source_version().id() == source.program_version_id() &&
               receipt->source_bundle().id() == source.bundle_id() &&
               receipt->target_version().id() == target.program_version_id() &&
               receipt->target_bundle().id() == target.bundle_id() &&
               source.binding_fingerprint() == capability_binding_receipt_root(
                   receipt->source_version().core_materialization_receipt()
                       .capability_bindings) &&
               source.id() == previous_lineage.active_run_record_id() &&
               source.journal_head() == previous_lineage.active_journal_head() &&
               predecessor.id() == previous_lineage.active_generation_id() &&
               predecessor.run_id() == source.run_id() &&
               source.remaining_budget() == previous_lineage.remaining_budget() &&
               source.continuation().state == ContinuationState::Running &&
               source.continuation().attempt == target.continuation().attempt &&
               !source.recorded_binding_set_fingerprint() && !source.pending_input() &&
               !source.pending_effect() && !source.terminal_result() &&
               source.children().empty() && source.child_depth() == 0 &&
               source.effect_sequence() == 0 &&
               source.updated_at_ms() >= capsule.checkpoint().timestamp &&
               source.invocation().parent_run_id.empty() &&
               previous_lineage.inflight_reservation() == RunBudget{} &&
               previous_lineage.committed_descendant_budget() == RunBudget{} &&
               receipt->target_generation() == successor.generation() &&
               receipt->target_run_id() == target.run_id() &&
               receipt->target_program_version_id() == target.program_version_id() &&
               receipt->target_bundle_id() == target.bundle_id() &&
               receipt->target_invocation_id() ==
                   target.invocation().canonical_identity() &&
               receipt->target_binding_fingerprint() ==
                   target.binding_fingerprint() &&
               target.binding_fingerprint() == capability_binding_receipt_root(
                   receipt->target_version().core_materialization_receipt()
                       .capability_bindings) &&
               receipt->target_initial_run_record_id() == target.id() &&
               receipt->target_initial_journal_head() == target.journal_head() &&
               target_checkpoint &&
               *target_checkpoint == receipt->target_core_checkpoint() &&
               target.exact_checkpoint_content_id() &&
               *target.exact_checkpoint_content_id() ==
                   graph_migration_checkpoint_content_id(
                       receipt->target_checkpoint()) &&
               receipt->target_checkpoint().id == target_checkpoint->checkpoint_id &&
               receipt->target_checkpoint().thread_id ==
                   target_checkpoint->core_thread_id &&
               target_checkpoint->core_thread_id == program_root_core_thread_id(
                   target.run_id(), target_checkpoint->core_generation_id) &&
               target_checkpoint->checkpoint_id !=
                   capsule.core_checkpoint().checkpoint_id &&
               target.run_id() != source.run_id() && target.child_depth() == 0 &&
               target.invocation().parent_run_id.empty() &&
               same_migration_invocation(source.invocation(), target.invocation()) &&
               target.continuation().state == ContinuationState::Running &&
               !target.pending_input() && !target.pending_effect() &&
               !target.terminal_result() && !target.fork_receipt() &&
               !target.recorded_binding_set_fingerprint() &&
               target.children().empty() && target.effect_sequence() == 0 &&
                target.created_at_ms() >= source.updated_at_ms() &&
                migration_elapsed <= source.remaining_budget().wall_time_ms &&
                target.created_at_ms() >= capsule.checkpoint().timestamp &&
               successor.child_depth() == predecessor.child_depth() &&
               next_lineage.remaining_budget() == expected_budget &&
               next_lineage.inflight_reservation() == RunBudget{} &&
               next_lineage.committed_descendant_budget() == RunBudget{} &&
               target.remaining_budget() == expected_budget &&
               target.invocation().budget == expected_budget &&
               budget_at_most(expected_budget, source.invocation().budget) &&
               budget_at_most(
                   expected_budget,
                   receipt->target_version().policy_snapshot().budget_ceiling()) &&
               budget_fits_bundle(expected_budget, receipt->target_bundle());
    } catch (const std::exception&) {
        return false;
    }
}

bool does_program_graph_migration_started_event_bind(
    const ProgramEvent& event,
    const ProgramRunRecord& target) noexcept {
    try {
        const auto checkpoint = target.exact_checkpoint();
        const auto* started = std::get_if<ProgramStartedEvent>(&event.payload);
        return checkpoint && started && event.kind == ProgramEventKind::Started &&
               event.sequence == 1 && event.timestamp_ms == target.created_at_ms() &&
               event.run_id == target.run_id() &&
               event.program_version_id == target.program_version_id() &&
               event.bundle_id == target.bundle_id() &&
               event.operation_id == target.continuation().operation_id &&
               event.core_generation_id == checkpoint->core_generation_id &&
               event.core_run_id == checkpoint->core_thread_id &&
               event.trace_id == target.invocation().correlation_id &&
               event.attempt == target.continuation().attempt &&
               started->budget == target.remaining_budget();
    } catch (const std::exception&) {
        return false;
    }
}

bool is_valid_program_graph_safe_point_transition(
    const ProgramRunRecord& previous,
    const ProgramJournalRecord& previous_journal,
    const ProgramTransitionPublication& publication,
    const ProgramGraphSafePointEvidence& evidence,
    const GraphMigrationCapsule& capsule,
    const ProgramExecutionLease& execution_lease) noexcept {
    try {
        const auto& next = publication.run_record;
        const auto& next_journal = publication.journal_record;
        const auto checkpoint = next.exact_checkpoint();
        if (!checkpoint || !next.exact_checkpoint_content_id() ||
            publication.events.empty() || !publication.commands.empty() ||
            !publication.effects.empty() ||
            publication.run_generation || !publication.run_lineage ||
            publication.fork_source_lineage ||
            previous.continuation().state != ContinuationState::Running ||
            next.continuation().state != ContinuationState::Running ||
            previous.continuation() != next.continuation() ||
            previous.owner_scope() != next.owner_scope() ||
            previous.run_id() != next.run_id() ||
            previous.program_version_id() != next.program_version_id() ||
            previous.bundle_id() != next.bundle_id() ||
            previous.binding_fingerprint() != next.binding_fingerprint() ||
            previous.invocation() != next.invocation() ||
            previous.child_depth() != 0 || next.child_depth() != 0 ||
            !previous.invocation().parent_run_id.empty() ||
            previous.pending_input() || previous.pending_effect() ||
            previous.terminal_result() || !previous.children().empty() ||
            previous.effect_sequence() != 0 || next.pending_input() ||
            next.pending_effect() || next.terminal_result() ||
            !next.children().empty() || next.effect_sequence() != 0 ||
            previous.recorded_binding_set_fingerprint() ||
            next.recorded_binding_set_fingerprint() ||
            previous_journal.id != previous.journal_head() ||
            next_journal.previous_id != previous_journal.id ||
            previous_journal.sequence == std::numeric_limits<std::uint64_t>::max() ||
            next_journal.sequence != previous_journal.sequence + 1 ||
            next_journal.core_checkpoint != checkpoint ||
            previous_journal.inflight_reservation != RunBudget{} ||
            next_journal.inflight_reservation != RunBudget{} ||
            next.remaining_budget() != next_journal.remaining_budget ||
            !budget_at_most(next.remaining_budget(), previous.remaining_budget()) ||
            next.created_at_ms() != previous.created_at_ms() ||
            next.updated_at_ms() < previous.updated_at_ms() ||
            next.updated_at_ms() < publication.events.back().timestamp_ms ||
            publication.events.back().kind != ProgramEventKind::CheckpointPublished) {
            return false;
        }
        const auto& checkpoint_event = publication.events.back();
        if (checkpoint_event.operation_id != next.continuation().operation_id ||
            checkpoint_event.core_generation_id != checkpoint->core_generation_id ||
            checkpoint_event.core_run_id != checkpoint->core_thread_id ||
            checkpoint_event.trace_id != next.invocation().correlation_id ||
            checkpoint_event.attempt != next.continuation().attempt ||
            std::get<ProgramCheckpointEvent>(checkpoint_event.payload).checkpoint !=
                *checkpoint) {
            return false;
        }
        if (previous.exact_checkpoint()) {
            const auto& old = *previous.exact_checkpoint();
            if (old.core_name != checkpoint->core_name ||
                old.core_generation_id != checkpoint->core_generation_id ||
                old.core_thread_id != checkpoint->core_thread_id ||
                old.checkpoint_id == checkpoint->checkpoint_id) {
                return false;
            }
        }
        const auto& snapshot = evidence.safe_point().checkpoint();
        const auto& generation = evidence.safe_point().generation();
        return execution_lease.owner_scope() == previous.owner_scope() &&
               execution_lease.run_id() == previous.run_id() &&
               execution_lease.attempt() == previous.continuation().attempt &&
               execution_lease.program_version_id() == previous.program_version_id() &&
               execution_lease.bundle_id() == previous.bundle_id() &&
               execution_lease.core_name() == checkpoint->core_name &&
               execution_lease.core_generation_id() == checkpoint->core_generation_id &&
               execution_lease.core_thread_id() == checkpoint->core_thread_id &&
               execution_lease.expires_at_ms() > next.updated_at_ms() &&
               capsule.owner_scope() == next.owner_scope() &&
               capsule.lineage_id() == publication.run_lineage->lineage_id() &&
               capsule.source_generation() ==
                   publication.run_lineage->active_generation() &&
               capsule.source_generation_id() ==
                   publication.run_lineage->active_generation_id() &&
               capsule.source_lineage_head_id() == publication.run_lineage->id() &&
               capsule.source_run_id() == next.run_id() &&
               capsule.source_program_version_id() == next.program_version_id() &&
               capsule.source_bundle_id() == next.bundle_id() &&
               capsule.core_checkpoint() == *checkpoint &&
               graph_migration_checkpoint_content_id(capsule.checkpoint()) ==
                   graph_migration_checkpoint_content_id(snapshot) &&
               snapshot.id == checkpoint->checkpoint_id &&
               snapshot.thread_id == checkpoint->core_thread_id &&
               snapshot.schema_version == checkpoint->checkpoint_schema_version &&
               snapshot.schema_version == graph::CHECKPOINT_SCHEMA_VERSION &&
               snapshot.interrupt_phase == graph::CheckpointPhase::Completed &&
               !snapshot.next_nodes.empty() && snapshot.step >= 0 &&
               snapshot.timestamp >= 0 && snapshot.timestamp <= next.updated_at_ms() &&
               generation.core_name == checkpoint->core_name &&
               generation.core_generation_id == checkpoint->core_generation_id &&
               evidence.version().ownership_scope() == next.owner_scope() &&
               evidence.version().id() == next.program_version_id() &&
               evidence.version().bundle_id() == next.bundle_id() &&
               evidence.bundle().id() == next.bundle_id() &&
               checkpoint->core_thread_id == program_root_core_thread_id(
                   next.run_id(), checkpoint->core_generation_id) &&
               *next.exact_checkpoint_content_id() ==
                   graph_migration_checkpoint_content_id(snapshot);
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace neograph::program
