#include <neograph/program/fork.h>

#include "canonical_json.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::string_view FORK_RECEIPT_FORMAT = "neograph-program-fork-compatibility";

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Fork receipt field '" + owned_key + "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Fork receipt field '" + owned_key + "' must be unsigned");
    }
    const auto number = value[owned_key].get<unsigned long long>();
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Fork receipt integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

json require_value(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key)) {
        throw std::invalid_argument("Fork receipt requires field '" + owned_key + "'");
    }
    return value[owned_key];
}

void require_token(std::string_view value, std::string_view name) {
    detail::validate_token(value, name);
}

void require_sha256(std::string_view value, std::string_view name) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(name) + " must be a sha256 identity");
    }
}

json encode_witness(const ForkCompatibilityWitness& witness) {
    return json{{"field", std::string(to_string(witness.field))},
                {"subject", witness.subject},
                {"source", witness.source},
                {"target", witness.target}};
}

ForkCompatibilityWitness parse_witness(const json& value) {
    if (!value.is_object()) {
        throw std::invalid_argument("Fork compatibility witness must be an object");
    }
    detail::reject_unknown_fields(value, "Fork compatibility witness",
                                  {"field", "subject", "source", "target"});
    ForkCompatibilityWitness witness;
    witness.field   = fork_compatibility_field_from_string(require_string(value, "field"));
    witness.subject = require_string(value, "subject");
    witness.source  = require_value(value, "source");
    witness.target  = require_value(value, "target");
    require_token(witness.subject, "Fork compatibility witness subject");
    return witness;
}

void normalize_receipt_data(ForkCompatibilityReceiptData& data) {
    require_token(data.owner_scope, "Fork receipt owner scope");
    require_token(data.source_run_id, "Fork receipt source run id");
    require_sha256(data.source_program_version_id, "Fork receipt source version id");
    require_token(data.source_checkpoint_id, "Fork receipt source checkpoint id");
    require_sha256(data.target_program_version_id, "Fork receipt target version id");
    for (const auto& witness : data.witnesses) {
        require_token(witness.subject, "Fork compatibility witness subject");
    }
    const bool rejected = !data.witnesses.empty();
    if ((data.status == ForkCompatibilityStatus::Rejected) != rejected) {
        throw std::invalid_argument(
            "Fork compatibility receipt status must agree with its witnesses");
    }
}

json receipt_body(const ForkCompatibilityReceiptData& data) {
    json witnesses = json::array();
    for (const auto& witness : data.witnesses) {
        witnesses.push_back(encode_witness(witness));
    }
    return json{{"owner_scope", data.owner_scope},
                {"source_run_id", data.source_run_id},
                {"source_program_version_id", data.source_program_version_id},
                {"source_checkpoint_id", data.source_checkpoint_id},
                {"target_program_version_id", data.target_program_version_id},
                {"status", std::string(to_string(data.status))},
                {"witnesses", std::move(witnesses)}};
}

json receipt_envelope(const ForkCompatibilityReceiptData& data) {
    auto value                      = receipt_body(data);
    value["format"]                 = std::string(FORK_RECEIPT_FORMAT);
    value["storage_schema_version"] = ForkCompatibilityReceipt::STORAGE_SCHEMA_VERSION;
    return value;
}

ForkCompatibilityReceiptData parse_receipt_body(const json& value) {
    const auto& witnesses = require_value(value, "witnesses");
    if (!witnesses.is_array()) {
        throw std::invalid_argument("Fork receipt witnesses must be an array");
    }
    ForkCompatibilityReceiptData data;
    data.owner_scope               = require_string(value, "owner_scope");
    data.source_run_id             = require_string(value, "source_run_id");
    data.source_program_version_id = require_string(value, "source_program_version_id");
    data.source_checkpoint_id      = require_string(value, "source_checkpoint_id");
    data.target_program_version_id = require_string(value, "target_program_version_id");
    data.status = fork_compatibility_status_from_string(require_string(value, "status"));
    data.witnesses.reserve(witnesses.size());
    for (const auto& witness : witnesses) {
        data.witnesses.push_back(parse_witness(witness));
    }
    normalize_receipt_data(data);
    return data;
}

enum class DefinitionMemberKind { Object, Array };

json require_definition_member(const SealedCoreDefinition& definition,
                               std::string_view            key,
                               DefinitionMemberKind        kind) {
    const std::string owned_key(key);
    if (!definition.definition.is_object() || !definition.definition.contains(owned_key)) {
        throw std::invalid_argument("Sealed Core definition '" + definition.name +
                                    "' has invalid " + owned_key);
    }
    const auto member = definition.definition[owned_key];
    const bool valid =
        kind == DefinitionMemberKind::Object ? member.is_object() : member.is_array();
    if (!valid) {
        throw std::invalid_argument("Sealed Core definition '" + definition.name +
                                    "' has invalid " + owned_key);
    }
    return member;
}
json definition_array(const SealedCoreDefinition& definition, std::string_view key) {
    if (!definition.definition.is_object()) {
        throw std::invalid_argument("Sealed Core definition '" + definition.name +
                                    "' is not an object");
    }
    if (!definition.definition.contains(std::string(key))) return json::array();
    return require_definition_member(definition, key, DefinitionMemberKind::Array);
}

std::set<std::string> object_keys(const json& object) {
    std::set<std::string> result;
    for (auto it = object.begin(); it != object.end(); ++it) {
        result.insert(it.key());
    }
    return result;
}

std::string reducer_name(const json& channel) {
    if (!channel.is_object() || !channel.contains("reducer") || !channel["reducer"].is_string()) {
        throw std::invalid_argument("Sealed Core channel requires a string reducer");
    }
    return channel["reducer"].get<std::string>();
}

bool continuation_node_exists(const json& nodes, std::string_view name) {
    return name.empty() || name == "__start__" || name == "__end__" ||
           nodes.contains(std::string(name));
}

void add_witness(std::vector<ForkCompatibilityWitness>& witnesses,
                 ForkCompatibilityField                 field,
                 std::string                            subject,
                 json                                   source,
                 json                                   target) {
    witnesses.push_back(
        ForkCompatibilityWitness{field, std::move(subject), std::move(source), std::move(target)});
}

void validate_sealed_definition(const SealedCoreDefinition& definition) {
    require_token(definition.name, "Sealed Core definition name");
    require_sha256(definition.definition_hash, "Sealed Core definition hash");
    if (sealed_core_definition_hash(definition.definition) != definition.definition_hash) {
        throw std::invalid_argument("Sealed Core definition hash does not match its content");
    }
    (void)require_definition_member(definition, "channels", DefinitionMemberKind::Object);
    (void)require_definition_member(definition, "nodes", DefinitionMemberKind::Object);
    (void)definition_array(definition, "edges");
    (void)definition_array(definition, "conditional_edges");
}

}  // namespace

std::string_view to_string(ForkCompatibilityField field) noexcept {
    switch (field) {
        case ForkCompatibilityField::OwnerScope:
            return "owner_scope";
        case ForkCompatibilityField::SourceRun:
            return "source_run";
        case ForkCompatibilityField::SourceCheckpoint:
            return "source_checkpoint";
        case ForkCompatibilityField::TargetVersion:
            return "target_version";
        case ForkCompatibilityField::CoreName:
            return "core_name";
        case ForkCompatibilityField::CoreGeneration:
            return "core_generation";
        case ForkCompatibilityField::CheckpointSchema:
            return "checkpoint_schema";
        case ForkCompatibilityField::Channel:
            return "channel";
        case ForkCompatibilityField::Reducer:
            return "reducer";
        case ForkCompatibilityField::Continuation:
            return "continuation";
    }
    return "continuation";
}

ForkCompatibilityField fork_compatibility_field_from_string(std::string_view value) {
    if (value == "owner_scope") return ForkCompatibilityField::OwnerScope;
    if (value == "source_run") return ForkCompatibilityField::SourceRun;
    if (value == "source_checkpoint") return ForkCompatibilityField::SourceCheckpoint;
    if (value == "target_version") return ForkCompatibilityField::TargetVersion;
    if (value == "core_name") return ForkCompatibilityField::CoreName;
    if (value == "core_generation") return ForkCompatibilityField::CoreGeneration;
    if (value == "checkpoint_schema") return ForkCompatibilityField::CheckpointSchema;
    if (value == "channel") return ForkCompatibilityField::Channel;
    if (value == "reducer") return ForkCompatibilityField::Reducer;
    if (value == "continuation") return ForkCompatibilityField::Continuation;
    throw std::invalid_argument("Unknown fork compatibility field");
}

std::string_view to_string(ForkCompatibilityStatus status) noexcept {
    switch (status) {
        case ForkCompatibilityStatus::Compatible:
            return "compatible";
        case ForkCompatibilityStatus::Rejected:
            return "rejected";
    }
    return "rejected";
}

ForkCompatibilityStatus fork_compatibility_status_from_string(std::string_view value) {
    if (value == "compatible") return ForkCompatibilityStatus::Compatible;
    if (value == "rejected") return ForkCompatibilityStatus::Rejected;
    throw std::invalid_argument("Unknown fork compatibility status");
}

struct ForkCompatibilityReceipt::Impl {
    explicit Impl(ForkCompatibilityReceiptData value) : data(std::move(value)) {}
    ForkCompatibilityReceiptData data;
    std::string                  id;
};

ForkCompatibilityReceipt::ForkCompatibilityReceipt(ForkCompatibilityReceiptData data) {
    normalize_receipt_data(data);
    auto impl = std::make_shared<Impl>(std::move(data));
    impl->id  = detail::sha256_identity("program-fork-compatibility",
                                        detail::canonical_json_bytes(receipt_envelope(impl->data)));
    impl_     = std::move(impl);
}

ForkCompatibilityReceipt::ForkCompatibilityReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

ForkCompatibilityReceipt ForkCompatibilityReceipt::parse(std::string_view stored_bytes) {
    json value;
    try {
        value = detail::parse_json_strict(stored_bytes);
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored fork receipt JSON: ") +
                                    error.what());
    }
    if (!value.is_object() || require_string(value, "format") != FORK_RECEIPT_FORMAT) {
        throw std::invalid_argument("Stored fork receipt has unknown format");
    }
    detail::reject_unknown_fields(
        value, "Stored fork receipt",
        {"format", "storage_schema_version", "id", "owner_scope", "source_run_id",
         "source_program_version_id", "source_checkpoint_id", "target_program_version_id", "status",
         "witnesses"});
    if (require_uint32(value, "storage_schema_version") != STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Stored fork receipt schema version is unsupported");
    }
    ForkCompatibilityReceipt parsed(parse_receipt_body(value));
    const auto               stored_id = require_string(value, "id");
    if (!detail::is_sha256_identity(stored_id) || stored_id != parsed.id()) {
        throw std::invalid_argument("Stored fork receipt id does not match its content");
    }
    return parsed;
}

const std::string& ForkCompatibilityReceipt::id() const noexcept {
    return impl_->id;
}
const std::string& ForkCompatibilityReceipt::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}
const std::string& ForkCompatibilityReceipt::source_run_id() const noexcept {
    return impl_->data.source_run_id;
}
const std::string& ForkCompatibilityReceipt::source_program_version_id() const noexcept {
    return impl_->data.source_program_version_id;
}
const std::string& ForkCompatibilityReceipt::source_checkpoint_id() const noexcept {
    return impl_->data.source_checkpoint_id;
}
const std::string& ForkCompatibilityReceipt::target_program_version_id() const noexcept {
    return impl_->data.target_program_version_id;
}
ForkCompatibilityStatus ForkCompatibilityReceipt::status() const noexcept {
    return impl_->data.status;
}
const std::vector<ForkCompatibilityWitness>& ForkCompatibilityReceipt::witnesses() const noexcept {
    return impl_->data.witnesses;
}
bool ForkCompatibilityReceipt::compatible() const noexcept {
    return impl_->data.status == ForkCompatibilityStatus::Compatible;
}

std::string ForkCompatibilityReceipt::serialize_canonical() const {
    auto value  = receipt_envelope(impl_->data);
    value["id"] = impl_->id;
    return detail::canonical_json_bytes(value);
}

std::string compatibility_error_message(const ForkCompatibilityReceipt& receipt) {
    const auto& witnesses = receipt.witnesses();
    if (witnesses.empty()) return "Program fork is incompatible";
    return "Program fork is incompatible: " + std::string(to_string(witnesses.front().field)) +
           " '" + witnesses.front().subject + "'";
}

ProgramForkCompatibilityError::ProgramForkCompatibilityError(ForkCompatibilityReceipt receipt)
    : std::runtime_error(compatibility_error_message(receipt)), receipt_(std::move(receipt)) {
    if (receipt_.compatible()) {
        throw std::invalid_argument("ProgramForkCompatibilityError requires a rejected receipt");
    }
}

const ForkCompatibilityReceipt& ProgramForkCompatibilityError::receipt() const noexcept {
    return receipt_;
}

ForkCompatibilityReceipt check_exact_fork_compatibility(ExactForkCompatibilityFacts facts) {
    require_token(facts.owner_scope, "Fork owner scope");
    require_token(facts.requested_source.source_run_id, "Fork source run id");
    require_token(facts.requested_source.source_checkpoint_id, "Fork source checkpoint id");
    require_sha256(facts.source_program_version_id, "Fork source version id");
    require_sha256(facts.target_program_version_id, "Fork target version id");
    require_sha256(facts.resolved_target_program_version_id, "Resolved fork target version id");
    validate_sealed_definition(facts.source_core_definition);
    validate_sealed_definition(facts.target_core_definition);

    std::vector<ForkCompatibilityWitness> witnesses;
    if (facts.owner_scope != facts.source_owner_scope) {
        add_witness(witnesses, ForkCompatibilityField::OwnerScope, "source",
                    facts.source_owner_scope, facts.owner_scope);
    }
    if (facts.owner_scope != facts.target_owner_scope) {
        add_witness(witnesses, ForkCompatibilityField::OwnerScope, "target",
                    facts.target_owner_scope, facts.owner_scope);
    }
    if (facts.requested_source.source_run_id != facts.stored_source_run_id) {
        add_witness(witnesses, ForkCompatibilityField::SourceRun, "run_id",
                    facts.stored_source_run_id, facts.requested_source.source_run_id);
    }
    if (facts.requested_source.source_checkpoint_id != facts.published_checkpoint.checkpoint_id) {
        add_witness(witnesses, ForkCompatibilityField::SourceCheckpoint, "published_id",
                    facts.published_checkpoint.checkpoint_id,
                    facts.requested_source.source_checkpoint_id);
    }
    if (facts.loaded_checkpoint.id != facts.published_checkpoint.checkpoint_id) {
        add_witness(witnesses, ForkCompatibilityField::SourceCheckpoint, "loaded_id",
                    facts.published_checkpoint.checkpoint_id, facts.loaded_checkpoint.id);
    }
    if (facts.loaded_checkpoint.thread_id != facts.published_checkpoint.core_thread_id) {
        add_witness(witnesses, ForkCompatibilityField::SourceCheckpoint, "core_thread_id",
                    facts.published_checkpoint.core_thread_id, facts.loaded_checkpoint.thread_id);
    }
    if (facts.target_program_version_id != facts.resolved_target_program_version_id) {
        add_witness(witnesses, ForkCompatibilityField::TargetVersion, "version_id",
                    facts.target_program_version_id, facts.resolved_target_program_version_id);
    }

    if (facts.source_core_plan.name != facts.source_core_definition.name ||
        facts.published_checkpoint.core_name != facts.source_core_plan.name) {
        add_witness(witnesses, ForkCompatibilityField::CoreName, "source",
                    json{{"plan", facts.source_core_plan.name},
                         {"definition", facts.source_core_definition.name}},
                    facts.published_checkpoint.core_name);
    }
    if (facts.target_core_plan.name != facts.target_core_definition.name ||
        facts.target_core_plan.name != facts.published_checkpoint.core_name) {
        add_witness(witnesses, ForkCompatibilityField::CoreName, "target",
                    facts.published_checkpoint.core_name,
                    json{{"plan", facts.target_core_plan.name},
                         {"definition", facts.target_core_definition.name}});
    }
    if (facts.published_checkpoint.core_generation_id !=
        facts.source_core_plan.compiled_plan_identity) {
        add_witness(witnesses, ForkCompatibilityField::CoreGeneration, "source",
                    facts.source_core_plan.compiled_plan_identity,
                    facts.published_checkpoint.core_generation_id);
    }
    if (facts.target_core_plan.compiled_plan_identity !=
        facts.source_core_plan.compiled_plan_identity) {
        add_witness(witnesses, ForkCompatibilityField::CoreGeneration, "target",
                    facts.source_core_plan.compiled_plan_identity,
                    facts.target_core_plan.compiled_plan_identity);
    }
    if (facts.loaded_checkpoint.schema_version !=
        facts.published_checkpoint.checkpoint_schema_version) {
        add_witness(witnesses, ForkCompatibilityField::CheckpointSchema, "loaded",
                    facts.published_checkpoint.checkpoint_schema_version,
                    facts.loaded_checkpoint.schema_version);
    }
    if (facts.published_checkpoint.checkpoint_schema_version != graph::CHECKPOINT_SCHEMA_VERSION) {
        add_witness(witnesses, ForkCompatibilityField::CheckpointSchema, "target",
                    facts.published_checkpoint.checkpoint_schema_version,
                    graph::CHECKPOINT_SCHEMA_VERSION);
    }

    const auto source_channels = require_definition_member(facts.source_core_definition, "channels",
                                                           DefinitionMemberKind::Object);
    const auto target_channels = require_definition_member(facts.target_core_definition, "channels",
                                                           DefinitionMemberKind::Object);
    const auto source_channel_names     = object_keys(source_channels);
    const auto target_channel_names     = object_keys(target_channels);
    std::set<std::string> channel_names = source_channel_names;
    channel_names.insert(target_channel_names.begin(), target_channel_names.end());

    for (const auto& name : channel_names) {
        const bool in_source = source_channels.contains(name);
        const bool in_target = target_channels.contains(name);
        if (in_source != in_target) {
            add_witness(witnesses, ForkCompatibilityField::Channel, name, in_source, in_target);
            continue;
        }
        const auto source_reducer = reducer_name(source_channels.at(name));
        const auto target_reducer = reducer_name(target_channels.at(name));
        if (source_reducer != target_reducer) {
            add_witness(witnesses, ForkCompatibilityField::Reducer, name, source_reducer,
                        target_reducer);
        }
        const bool has_checkpoint_value = facts.loaded_checkpoint.channel_values.is_object() &&
                                          facts.loaded_checkpoint.channel_values.contains(name);
        const bool has_checkpoint_version = facts.loaded_checkpoint.channel_versions.is_object() &&
                                            facts.loaded_checkpoint.channel_versions.contains(name);
        if (has_checkpoint_value != has_checkpoint_version) {
            add_witness(witnesses, ForkCompatibilityField::Channel, name,
                        json{{"checkpoint_value", has_checkpoint_value},
                             {"checkpoint_version", has_checkpoint_value}},
                        json{{"checkpoint_value", has_checkpoint_value},
                             {"checkpoint_version", has_checkpoint_version}});
        }
    }

    const auto source_nodes = require_definition_member(facts.source_core_definition, "nodes",
                                                        DefinitionMemberKind::Object);
    const auto target_nodes = require_definition_member(facts.target_core_definition, "nodes",
                                                        DefinitionMemberKind::Object);
    if (facts.source_continuation.state != ContinuationState::Interrupted) {
        add_witness(witnesses, ForkCompatibilityField::Continuation, "state",
                    std::string(to_string(facts.source_continuation.state)), "interrupted");
    }
    if (!continuation_node_exists(target_nodes, facts.loaded_checkpoint.current_node)) {
        add_witness(witnesses, ForkCompatibilityField::Continuation, "current_node",
                    facts.loaded_checkpoint.current_node, nullptr);
    }
    for (const auto& next : facts.loaded_checkpoint.next_nodes) {
        if (!continuation_node_exists(target_nodes, next)) {
            add_witness(witnesses, ForkCompatibilityField::Continuation, "next_node", next,
                        nullptr);
        }
    }
    if (source_nodes != target_nodes) {
        add_witness(witnesses, ForkCompatibilityField::Continuation, "nodes", source_nodes,
                    target_nodes);
    }
    const auto source_edges = definition_array(facts.source_core_definition, "edges");
    const auto target_edges = definition_array(facts.target_core_definition, "edges");
    if (source_edges != target_edges) {
        add_witness(witnesses, ForkCompatibilityField::Continuation, "edges", source_edges,
                    target_edges);
    }
    const auto source_conditions =
        definition_array(facts.source_core_definition, "conditional_edges");
    const auto target_conditions =
        definition_array(facts.target_core_definition, "conditional_edges");
    if (source_conditions != target_conditions) {
        add_witness(witnesses, ForkCompatibilityField::Continuation, "conditional_edges",
                    source_conditions, target_conditions);
    }

    ForkCompatibilityReceiptData receipt;
    receipt.owner_scope               = std::move(facts.owner_scope);
    receipt.source_run_id             = std::move(facts.stored_source_run_id);
    receipt.source_program_version_id = std::move(facts.source_program_version_id);
    receipt.source_checkpoint_id      = std::move(facts.published_checkpoint.checkpoint_id);
    receipt.target_program_version_id = std::move(facts.target_program_version_id);
    receipt.status =
        witnesses.empty() ? ForkCompatibilityStatus::Compatible : ForkCompatibilityStatus::Rejected;
    receipt.witnesses = std::move(witnesses);
    return ForkCompatibilityReceipt(std::move(receipt));
}

}  // namespace neograph::program
