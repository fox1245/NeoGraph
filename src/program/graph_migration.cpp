#include <neograph/program/graph_migration.h>

#include <neograph/graph/compiler.h>
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
constexpr std::string_view ADAPTER_FORMAT = "neograph-graph-semantic-migration-adapter";
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

struct SemanticAdapterData {
    std::string              owner_scope;
    std::string              source_program_version_id;
    std::string              source_bundle_id;
    std::string              target_program_version_id;
    std::string              target_bundle_id;
    std::string              source_core_name;
    std::string              target_core_name;
    std::string              source_core_generation_id;
    std::string              target_core_generation_id;
    std::string              topology_shape_id;
    std::vector<std::string> node_names;
    std::vector<std::string> checkpoint_channel_names;
    std::vector<std::string> barrier_names;
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

json string_array(const std::vector<std::string>& values) {
    json result = json::array();
    for (const auto& value : values) result.push_back(value);
    return result;
}

std::vector<std::string> parse_token_array(const json& value, std::string_view field) {
    if (!value.is_array()) {
        throw std::invalid_argument("Graph semantic migration adapter field '" +
                                    std::string(field) + "' must be an array");
    }
    std::vector<std::string> result;
    result.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw std::invalid_argument("Graph semantic migration adapter token array is invalid");
        }
        result.push_back(item.get<std::string>());
    }
    for (const auto& item : result)
        detail::validate_token(item, "Graph semantic migration adapter topology token");
    if (!std::is_sorted(result.begin(), result.end()) ||
        std::adjacent_find(result.begin(), result.end()) != result.end()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter topology tokens must be sorted and unique");
    }
    return result;
}

struct NativeRootCore {
    std::string            name;
    std::string            compiled_plan_identity;
    SealedCoreDefinition   definition;
};

NativeRootCore native_root_core(const ProgramVersion& version, const ProgramBundle& bundle) {
    if (version.bundle_id() != bundle.id() || bundle.control_source()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter requires an admitted native Program bundle");
    }
    const auto& plan = bundle.typed_orchestration_plan();
    if (plan.nodes().size() != 1 || plan.root().operation() != ProgramOperationKind::CallCore ||
        !plan.root().core()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter requires a native single-root CallCore plan");
    }
    NativeRootCore result;
    result.name = *plan.root().core();
    const auto materialized = std::find_if(
        version.core_materialization_receipt().plans.begin(),
        version.core_materialization_receipt().plans.end(), [&](const CorePlanIdentity& candidate) {
            return candidate.name == result.name;
        });
    auto definitions = bundle.sealed_core_definitions();
    const auto definition = std::find_if(
        definitions.begin(), definitions.end(),
        [&](const SealedCoreDefinition& candidate) { return candidate.name == result.name; });
    if (materialized == version.core_materialization_receipt().plans.end() ||
        definition == definitions.end()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter root does not bind its admitted materialization");
    }
    result.compiled_plan_identity = materialized->compiled_plan_identity;
    result.definition             = *definition;
    return result;
}

struct TopologyProjection {
    std::string              shape_id;
    std::vector<std::string> node_names;
    std::vector<std::string> checkpoint_channel_names;
    std::vector<std::string> barrier_names;
};

TopologyProjection topology_projection(const SealedCoreDefinition& definition) {
    const auto topology = graph::GraphCompiler::parse(definition.definition);
    json       shape = json::object();
    for (const auto& [key, value] : topology.to_json().items()) {
        if (key != "name") shape[key] = value;
    }
    json nodes = json::object();
    std::vector<std::string> node_names;
    node_names.reserve(topology.node_defs.size());
    for (const auto& [name, unused] : topology.node_defs) {
        (void)unused;
        node_names.push_back(name);
        nodes[name] = json::object();
    }
    shape["nodes"] = std::move(nodes);

    std::vector<std::string> checkpoint_channels;
    for (const auto& channel : topology.channel_defs) {
        if (channel.persistence == graph::ChannelPersistencePolicy::Checkpoint)
            checkpoint_channels.push_back(channel.name);
    }
    std::vector<std::string> barriers;
    barriers.reserve(topology.barrier_specs.size());
    for (const auto& [name, unused] : topology.barrier_specs) {
        (void)unused;
        barriers.push_back(name);
    }
    return {detail::sha256_identity("graph-semantic-topology-shape/v1",
                                    detail::canonical_json_bytes(shape)),
            std::move(node_names), std::move(checkpoint_channels), std::move(barriers)};
}

json semantic_adapter_body(const SemanticAdapterData& data) {
    return {{"format", std::string(ADAPTER_FORMAT)},
            {"storage_schema_version", GraphSemanticMigrationAdapter::STORAGE_SCHEMA_VERSION},
            {"owner_scope", data.owner_scope},
            {"source_program_version_id", data.source_program_version_id},
            {"source_bundle_id", data.source_bundle_id},
            {"target_program_version_id", data.target_program_version_id},
            {"target_bundle_id", data.target_bundle_id},
            {"source_core_name", data.source_core_name},
            {"target_core_name", data.target_core_name},
            {"source_core_generation_id", data.source_core_generation_id},
            {"target_core_generation_id", data.target_core_generation_id},
            {"topology_shape_id", data.topology_shape_id},
            {"node_names", string_array(data.node_names)},
            {"checkpoint_channel_names", string_array(data.checkpoint_channel_names)},
            {"barrier_names", string_array(data.barrier_names)}};
}

void validate_semantic_adapter_data(const SemanticAdapterData& data) {
    detail::validate_token(data.owner_scope, "Graph semantic migration adapter owner scope");
    for (const auto* identity : {&data.source_program_version_id, &data.source_bundle_id,
                                 &data.target_program_version_id, &data.target_bundle_id,
                                 &data.source_core_generation_id,
                                 &data.target_core_generation_id, &data.topology_shape_id}) {
        require_identity(*identity, "Graph semantic migration adapter identity");
    }
    for (const auto* token : {&data.source_core_name, &data.target_core_name})
        detail::validate_token(*token, "Graph semantic migration adapter Core name");
    for (const auto* values : {&data.node_names, &data.checkpoint_channel_names,
                               &data.barrier_names}) {
        for (const auto& value : *values)
            detail::validate_token(value, "Graph semantic migration adapter topology token");
        if (!std::is_sorted(values->begin(), values->end()) ||
            std::adjacent_find(values->begin(), values->end()) != values->end()) {
            throw std::invalid_argument(
                "Graph semantic migration adapter topology tokens must be sorted and unique");
        }
    }
    if (data.node_names.empty()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter requires a nonempty native topology");
    }
    if (data.source_program_version_id == data.target_program_version_id) {
        throw std::invalid_argument(
            "Graph semantic migration adapter requires distinct admitted ProgramVersions");
    }
}

SemanticAdapterData prepare_semantic_adapter_data(const ProgramVersion& source_version,
                                                  const ProgramBundle&  source_bundle,
                                                  const ProgramVersion& target_version,
                                                  const ProgramBundle&  target_bundle) {
    const auto source_root = native_root_core(source_version, source_bundle);
    const auto target_root = native_root_core(target_version, target_bundle);
    if (source_version.ownership_scope() != target_version.ownership_scope() ||
        source_bundle.capability_effect_closure() != target_bundle.capability_effect_closure() ||
        source_bundle.execution_guarantee() != target_bundle.execution_guarantee() ||
        source_bundle.executable_registry_identities() !=
            target_bundle.executable_registry_identities() ||
        source_version.core_materialization_receipt().compiler_build_id !=
            target_version.core_materialization_receipt().compiler_build_id ||
        source_version.core_materialization_receipt().registry_snapshot_fingerprint !=
            target_version.core_materialization_receipt().registry_snapshot_fingerprint ||
        source_version.core_materialization_receipt().capability_bindings !=
            target_version.core_materialization_receipt().capability_bindings ||
        source_bundle.input_contract().schema_version != target_bundle.input_contract().schema_version ||
        detail::canonical_json_bytes(source_bundle.input_contract().schema) !=
            detail::canonical_json_bytes(target_bundle.input_contract().schema) ||
        source_bundle.output_contract().schema_version != target_bundle.output_contract().schema_version ||
        detail::canonical_json_bytes(source_bundle.output_contract().schema) !=
            detail::canonical_json_bytes(target_bundle.output_contract().schema)) {
        throw std::invalid_argument(
            "Graph semantic migration adapter changes authority, bindings, or contracts");
    }
    const auto source_topology = topology_projection(source_root.definition);
    const auto target_topology = topology_projection(target_root.definition);
    if (source_topology.shape_id != target_topology.shape_id ||
        source_topology.node_names != target_topology.node_names ||
        source_topology.checkpoint_channel_names != target_topology.checkpoint_channel_names ||
        source_topology.barrier_names != target_topology.barrier_names) {
        throw std::invalid_argument(
            "Graph semantic migration adapter requires an identity-mapped topology shape");
    }
    const auto baseline = MigrationPlan::between(source_version, source_bundle,
                                                 target_version, target_bundle);
    for (const auto& diagnostic : baseline.diagnostics()) {
        if (diagnostic.code != "core_materialization_receipt" &&
            diagnostic.code != "bundle_runtime_contract") {
            throw std::invalid_argument(
                "Graph semantic migration adapter cannot override migration diagnostic '" +
                diagnostic.code + "'");
        }
    }
    SemanticAdapterData data{source_version.ownership_scope(),
                             source_version.id(),
                             source_bundle.id(),
                             target_version.id(),
                             target_bundle.id(),
                             source_root.name,
                             target_root.name,
                             source_root.compiled_plan_identity,
                             target_root.compiled_plan_identity,
                             source_topology.shape_id,
                             source_topology.node_names,
                             source_topology.checkpoint_channel_names,
                             source_topology.barrier_names};
    validate_semantic_adapter_data(data);
    return data;
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
    if (data.semantic_adapter && !data.semantic_adapter->binds(
                                     data.source_version, data.source_bundle,
                                     data.target_version, data.target_bundle)) {
        throw std::invalid_argument(
            "Program Graph migration semantic adapter does not bind admitted artifacts");
    }
    const auto expected_plan = data.semantic_adapter
        ? data.semantic_adapter->migration_plan(data.source_version, data.source_bundle,
                                                data.target_version, data.target_bundle)
        : MigrationPlan::between(data.source_version, data.source_bundle,
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
        (!data.semantic_adapter && target.core_name != capsule.core_checkpoint().core_name) ||
        (!data.semantic_adapter &&
         target.core_generation_id != capsule.core_checkpoint().core_generation_id) ||
        target_materialization ==
            data.target_version.core_materialization_receipt().plans.end()) {
        throw std::invalid_argument(
            "Program Graph migration target checkpoint is not an exact root clone");
    }
    const auto& snapshot = data.target_checkpoint;
    auto expected_snapshot = data.semantic_adapter
        ? data.semantic_adapter->project(capsule)
        : capsule.checkpoint();
    expected_snapshot.id = target.checkpoint_id;
    expected_snapshot.thread_id = target.core_thread_id;
    expected_snapshot.parent_id = capsule.core_checkpoint().checkpoint_id;
    expected_snapshot.metadata["migrated_from"] = capsule.id();
    if (data.semantic_adapter) {
        expected_snapshot.metadata["semantic_migration_adapter_id"] =
            data.semantic_adapter->id();
    }
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

json receipt_body(const ProgramGraphMigrationReceiptData& data,
                  std::uint32_t                          storage_schema_version) {
    json result{{"format", std::string(RECEIPT_FORMAT)},
                {"storage_schema_version", storage_schema_version},
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
    if (storage_schema_version >= ProgramGraphMigrationReceipt::STORAGE_SCHEMA_VERSION) {
        result["semantic_adapter"] = data.semantic_adapter
            ? detail::parse_json_strict(data.semantic_adapter->serialize_canonical())
            : json(nullptr);
    }
    return result;
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

struct GraphSemanticMigrationAdapter::Impl {
    explicit Impl(SemanticAdapterData value) : data(std::move(value)) {
        auto encoded = semantic_adapter_body(data);
        id = detail::sha256_identity("graph-semantic-migration-adapter/v1",
                                     detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    SemanticAdapterData data;
    std::string         id;
    std::string         canonical_bytes;
};

GraphSemanticMigrationAdapter::GraphSemanticMigrationAdapter(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

GraphSemanticMigrationAdapter GraphSemanticMigrationAdapter::prepare(
    const ProgramVersion& source_version, const ProgramBundle& source_bundle,
    const ProgramVersion& target_version, const ProgramBundle& target_bundle) {
    return GraphSemanticMigrationAdapter(std::make_shared<const Impl>(
        prepare_semantic_adapter_data(source_version, source_bundle, target_version, target_bundle)));
}

GraphSemanticMigrationAdapter GraphSemanticMigrationAdapter::parse(std::string_view stored_bytes) {
    const auto value = detail::parse_json_strict(stored_bytes);
    if (!value.is_object() || require_string(value, "format") != ADAPTER_FORMAT) {
        throw std::invalid_argument("Stored Graph semantic migration adapter has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored Graph semantic migration adapter",
        {"format", "storage_schema_version", "id", "owner_scope", "source_program_version_id",
         "source_bundle_id", "target_program_version_id", "target_bundle_id", "source_core_name",
         "target_core_name", "source_core_generation_id", "target_core_generation_id",
         "topology_shape_id", "node_names", "checkpoint_channel_names", "barrier_names"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored Graph semantic migration adapter schema is unsupported");
    }
    const auto require_array = [&](std::string_view field) -> json {
        const auto key = std::string(field);
        if (!value.contains(key)) {
            throw std::invalid_argument("Stored Graph semantic migration adapter is incomplete");
        }
        return value.at(key);
    };
    SemanticAdapterData data{require_string(value, "owner_scope"),
                             require_string(value, "source_program_version_id"),
                             require_string(value, "source_bundle_id"),
                             require_string(value, "target_program_version_id"),
                             require_string(value, "target_bundle_id"),
                             require_string(value, "source_core_name"),
                             require_string(value, "target_core_name"),
                             require_string(value, "source_core_generation_id"),
                             require_string(value, "target_core_generation_id"),
                             require_string(value, "topology_shape_id"),
                             parse_token_array(require_array("node_names"), "node_names"),
                             parse_token_array(require_array("checkpoint_channel_names"),
                                               "checkpoint_channel_names"),
                             parse_token_array(require_array("barrier_names"), "barrier_names")};
    validate_semantic_adapter_data(data);
    GraphSemanticMigrationAdapter result(std::make_shared<const Impl>(std::move(data)));
    if (result.id() != require_string(value, "id")) {
        throw std::invalid_argument("Stored Graph semantic migration adapter identity is invalid");
    }
    return result;
}

#define NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(name)                 \
    const std::string& GraphSemanticMigrationAdapter::name() const noexcept { \
        return impl_->data.name;                                         \
    }
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(owner_scope)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(source_program_version_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(source_bundle_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(target_program_version_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(target_bundle_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(source_core_name)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(target_core_name)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(source_core_generation_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(target_core_generation_id)
NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR(topology_shape_id)
#undef NEOGRAPH_SEMANTIC_ADAPTER_STRING_ACCESSOR

const std::string& GraphSemanticMigrationAdapter::id() const noexcept { return impl_->id; }

bool GraphSemanticMigrationAdapter::binds(const ProgramVersion& source_version,
                                          const ProgramBundle& source_bundle,
                                          const ProgramVersion& target_version,
                                          const ProgramBundle& target_bundle) const {
    try {
        const auto expected = prepare_semantic_adapter_data(
            source_version, source_bundle, target_version, target_bundle);
        return detail::canonical_json_bytes(semantic_adapter_body(impl_->data)) ==
               detail::canonical_json_bytes(semantic_adapter_body(expected));
    } catch (const std::exception&) {
        return false;
    }
}

graph::Checkpoint GraphSemanticMigrationAdapter::project(
    const GraphMigrationCapsule& capsule) const {
    if (capsule.owner_scope() != owner_scope() ||
        capsule.source_program_version_id() != source_program_version_id() ||
        capsule.source_bundle_id() != source_bundle_id() ||
        capsule.core_checkpoint().core_name != source_core_name() ||
        capsule.core_checkpoint().core_generation_id != source_core_generation_id()) {
        throw std::invalid_argument(
            "Graph semantic migration adapter does not bind the source capsule");
    }
    auto result = capsule.checkpoint();
    const auto& channels = result.channel_values.at("channels");
    for (const auto& [name, unused] : channels.items()) {
        (void)unused;
        if (!std::binary_search(impl_->data.checkpoint_channel_names.begin(),
                                impl_->data.checkpoint_channel_names.end(), name)) {
            throw std::invalid_argument(
                "Graph semantic migration adapter cannot project an unknown channel");
        }
    }
    for (const auto& name : impl_->data.checkpoint_channel_names) {
        if (!channels.contains(name)) {
            throw std::invalid_argument(
                "Graph semantic migration adapter requires every checkpoint channel");
        }
    }
    const auto known_node = [&](const std::string& name) {
        return name == "__start__" || name == "__end__" ||
               std::binary_search(impl_->data.node_names.begin(), impl_->data.node_names.end(), name);
    };
    if (!known_node(result.current_node)) {
        throw std::invalid_argument(
            "Graph semantic migration adapter cannot project an unknown current node");
    }
    for (const auto& node : result.next_nodes) {
        if (!known_node(node)) {
            throw std::invalid_argument(
                "Graph semantic migration adapter cannot project an unknown frontier node");
        }
    }
    for (const auto& [barrier, signals] : result.barrier_state) {
        if (!std::binary_search(impl_->data.barrier_names.begin(),
                                impl_->data.barrier_names.end(), barrier)) {
            throw std::invalid_argument(
                "Graph semantic migration adapter cannot project an unknown barrier");
        }
        for (const auto& signal : signals) {
            if (!known_node(signal)) {
                throw std::invalid_argument(
                    "Graph semantic migration adapter cannot project an unknown barrier signal");
            }
        }
    }
    return result;
}

MigrationPlan GraphSemanticMigrationAdapter::migration_plan(
    const ProgramVersion& source_version, const ProgramBundle& source_bundle,
    const ProgramVersion& target_version, const ProgramBundle& target_bundle) const {
    if (!binds(source_version, source_bundle, target_version, target_bundle)) {
        throw std::invalid_argument(
            "Graph semantic migration adapter does not bind the admitted versions");
    }
    const auto baseline = MigrationPlan::between(source_version, source_bundle,
                                                 target_version, target_bundle);
    std::vector<MigrationMapping> mappings = baseline.mappings();
    mappings.push_back(
        {MigrationDimension::Materialization, "shape_preserving_semantic_adapter",
         json{{"adapter_id", id()}, {"topology_shape_id", topology_shape_id()}},
         json{{"adapter_id", id()}, {"topology_shape_id", topology_shape_id()}}});
    return MigrationPlan::create(
        {source_version.id(), target_version.id(), source_version.ownership_scope(),
         MigrationCompatibility::ForkCompatible, {}, {}, std::move(mappings)});
}

std::string GraphSemanticMigrationAdapter::serialize_canonical() const {
    return impl_->canonical_bytes;
}

struct ProgramGraphMigrationReceipt::Impl {
    Impl(ProgramGraphMigrationReceiptData value, std::uint32_t storage_schema)
        : data(std::move(value)), storage_schema_version(storage_schema) {
        auto encoded = receipt_body(data, storage_schema_version);
        id = detail::sha256_identity(
            "program-graph-migration-receipt/v1",
            detail::canonical_json_bytes(encoded));
        encoded["id"] = id;
        canonical_bytes = detail::canonical_json_bytes(encoded);
    }

    ProgramGraphMigrationReceiptData data;
    std::uint32_t                     storage_schema_version = 0;
    std::string                      id;
    std::string                      canonical_bytes;
};

ProgramGraphMigrationReceipt::ProgramGraphMigrationReceipt(
    ProgramGraphMigrationReceiptData data) {
    validate_receipt(data);
    impl_ = std::make_shared<const Impl>(std::move(data), STORAGE_SCHEMA_VERSION);
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
          "semantic_adapter",
          "migration_plan_id", "target_generation", "target_run_id",
         "target_program_version_id", "target_bundle_id",
         "target_invocation_id", "target_binding_fingerprint",
         "target_initial_run_record_id", "target_initial_journal_head",
         "target_core_checkpoint", "target_checkpoint"});
    const auto schema_version = require_uint32(value, "storage_schema_version");
    if ((schema_version != STORAGE_SCHEMA_VERSION &&
         schema_version != PREVIOUS_STORAGE_SCHEMA_VERSION) || !value.contains("capsule") ||
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
    if (schema_version == STORAGE_SCHEMA_VERSION && !value.contains("semantic_adapter")) {
        throw std::invalid_argument(
            "Stored Program Graph migration receipt is missing its semantic adapter marker");
    }
    std::optional<GraphSemanticMigrationAdapter> semantic_adapter;
    if (schema_version != PREVIOUS_STORAGE_SCHEMA_VERSION &&
        !value.at("semantic_adapter").is_null()) {
        semantic_adapter.emplace(GraphSemanticMigrationAdapter::parse(
            detail::canonical_json_bytes(value.at("semantic_adapter"))));
    }
    ProgramGraphMigrationReceiptData data{
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
        parse_checkpoint(value.at("target_checkpoint")),
        std::move(semantic_adapter)};
    validate_receipt(data);
    ProgramGraphMigrationReceipt result(
        std::make_shared<const Impl>(std::move(data), schema_version));
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
const std::optional<GraphSemanticMigrationAdapter>&
ProgramGraphMigrationReceipt::semantic_adapter() const noexcept {
    return impl_->data.semantic_adapter;
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
        const auto expected_plan = receipt->semantic_adapter()
            ? receipt->semantic_adapter()->migration_plan(
                  receipt->source_version(), receipt->source_bundle(),
                  receipt->target_version(), receipt->target_bundle())
            : MigrationPlan::between(receipt->source_version(), receipt->source_bundle(),
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
