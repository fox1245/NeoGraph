#include <neograph/program/graph_migration.h>

#include "canonical_json.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORMAT = "neograph-graph-migration-capsule";

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
    return {{"format", std::string(FORMAT)},
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
    if (!value.is_object() || require_string(value, "format") != FORMAT) {
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

}  // namespace neograph::program
