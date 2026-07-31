#include <neograph/graph/node.h>
#include <neograph/program/registry.h>

#include "canonical_json.h"
#include "registry_access.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::uint32_t kStorageSchemaVersion = 1;

void require_nonempty(std::string_view value, std::string_view field) {
    detail::validate_token(value, field);
}

void require_digest(std::string_view value, std::string_view field) {
    if (!detail::is_sha256_identity(value)) {
        throw std::invalid_argument(std::string(field) + " must be a sha256: identity");
    }
}

void normalize_strings(std::vector<std::string>& values, std::string_view field) {
    for (const auto& value : values)
        require_nonempty(value, field);
    std::sort(values.begin(), values.end());
    if (std::adjacent_find(values.begin(), values.end()) != values.end()) {
        throw std::invalid_argument(std::string(field) + " contains a duplicate");
    }
}

void validate_identity(const ExecutableIdentity& identity) {
    require_nonempty(identity.name, "Executable name");
    if (to_string(identity.kind) == "unknown") {
        throw std::invalid_argument("Executable kind is unsupported");
    }
    if (!detail::is_semantic_version(identity.semantic_version)) {
        throw std::invalid_argument("Executable semantic_version is not valid SemVer");
    }
    require_digest(identity.implementation_digest, "Executable implementation_digest");
}
bool identity_less(const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
    return std::tuple{to_string(lhs.kind), lhs.name, lhs.semantic_version,
                      lhs.implementation_digest} < std::tuple{to_string(rhs.kind), rhs.name,
                                                              rhs.semantic_version,
                                                              rhs.implementation_digest};
}

void normalize_dependencies(ExecutableManifest& manifest) {
    for (const auto& dependency : manifest.required_executables) {
        validate_identity(dependency);
        if (dependency == manifest.identity) {
            throw std::invalid_argument("Executable manifest cannot depend on itself");
        }
    }
    std::sort(manifest.required_executables.begin(), manifest.required_executables.end(),
              identity_less);
    const auto duplicate_name = std::adjacent_find(
        manifest.required_executables.begin(), manifest.required_executables.end(),
        [](const ExecutableIdentity& lhs, const ExecutableIdentity& rhs) {
            return lhs.kind == rhs.kind && lhs.name == rhs.name;
        });
    if (duplicate_name != manifest.required_executables.end()) {
        if (*duplicate_name == *std::next(duplicate_name)) {
            throw std::invalid_argument("required_executables contains a duplicate identity");
        }
        throw std::invalid_argument(
            "required_executables contains ambiguous identities for one executable");
    }
}

void validate_manifest(ExecutableManifest& manifest) {
    if (to_string(manifest.effect_mode) == "unknown") {
        throw std::invalid_argument("Executable effect mode is unsupported");
    }
    validate_identity(manifest.identity);
    require_nonempty(manifest.attestation_id, "Executable attestation_id");
    normalize_strings(manifest.required_capabilities, "required_capabilities");
    normalize_strings(manifest.declared_effects, "declared_effects");
    normalize_dependencies(manifest);
}

void require_kind(const ExecutableManifest& manifest, ExecutableKind expected) {
    if (manifest.identity.kind != expected) {
        throw std::invalid_argument("Executable manifest kind does not match builder operation");
    }
}

struct SnapshotEntry {
    ExecutableManifest manifest;
    json               metadata;
};

json encode_identity(const ExecutableIdentity& identity) {
    return json{{"kind", std::string(to_string(identity.kind))},
                {"name", identity.name},
                {"semantic_version", identity.semantic_version},
                {"implementation_digest", identity.implementation_digest}};
}

json encode_manifest(const SnapshotEntry& entry) {
    const auto& manifest           = entry.manifest;
    json        value              = encode_identity(manifest.identity);
    value["effect_mode"]           = std::string(to_string(manifest.effect_mode));
    value["attestation_id"]        = manifest.attestation_id;
    value["required_capabilities"] = manifest.required_capabilities;
    value["declared_effects"]      = manifest.declared_effects;
    json dependencies              = json::array();
    for (const auto& dependency : manifest.required_executables)
        dependencies.push_back(encode_identity(dependency));
    value["required_executables"] = std::move(dependencies);
    value["metadata"]             = detail::owned_json_copy(entry.metadata);
    return value;
}

}  // namespace

struct RegistrySnapshot::Impl {
    graph::GraphRegistry       registry;
    std::vector<SnapshotEntry> entries;
    std::string                fingerprint;
    json                       manifest;
};

struct RegistrySnapshotBuilder::Impl {
    struct ImportedBinding {
        std::string        alias;
        ExecutableIdentity target;
    };

    graph::GraphRegistry         registry;
    std::vector<SnapshotEntry>   entries;
    std::vector<ImportedBinding> imported_targets;
};

RegistrySnapshot::RegistrySnapshot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}
RegistrySnapshot::~RegistrySnapshot() = default;

const std::string& RegistrySnapshot::fingerprint() const noexcept {
    return impl_->fingerprint;
}

std::vector<ExecutableIdentity> RegistrySnapshot::identities() const {
    std::vector<ExecutableIdentity> values;
    values.reserve(impl_->entries.size());
    for (const auto& entry : impl_->entries)
        values.push_back(entry.manifest.identity);
    return values;
}

std::optional<ExecutableManifest> RegistrySnapshot::find(ExecutableKind   kind,
                                                         std::string_view name) const {
    const auto it = std::find_if(
        impl_->entries.begin(), impl_->entries.end(), [kind, name](const SnapshotEntry& entry) {
            return entry.manifest.identity.kind == kind && entry.manifest.identity.name == name;
        });
    if (it == impl_->entries.end()) return std::nullopt;
    return it->manifest;
}

const ExecutableManifest& detail::RegistrySnapshotAccess::require_manifest(
    const RegistrySnapshot& snapshot, ExecutableKind kind, std::string_view name) {
    const auto manifest = std::find_if(
        snapshot.impl_->entries.begin(), snapshot.impl_->entries.end(),
        [kind, name](const SnapshotEntry& entry) {
            return entry.manifest.identity.kind == kind && entry.manifest.identity.name == name;
        });
    if (manifest == snapshot.impl_->entries.end()) {
        throw std::out_of_range("Executable manifest is absent from the RegistrySnapshot");
    }
    return manifest->manifest;
}

graph::TopologySpec detail::RegistrySnapshotAccess::parse_local(const RegistrySnapshot& snapshot,
                                                                const json& definition) {
    return graph::GraphCompiler::parse_local(definition, snapshot.impl_->registry);
}
graph::ParseReport detail::RegistrySnapshotAccess::parse_local_report(
    const RegistrySnapshot& snapshot, const json& definition) {
    return graph::GraphCompiler::parse_local_report(definition, snapshot.impl_->registry);
}

graph::RoundTripReport detail::RegistrySnapshotAccess::verify_roundtrip_report(
    const json& definition, const graph::TopologySpec& topology) {
    return graph::GraphCompiler::verify_roundtrip_report(definition, topology);
}

graph::ValidationReport detail::RegistrySnapshotAccess::validate_local(
    const RegistrySnapshot& snapshot, const graph::TopologySpec& topology) {
    return graph::GraphValidator::validate_local(topology, snapshot.impl_->registry);
}

graph::CompiledGraph detail::RegistrySnapshotAccess::link_local(const RegistrySnapshot&   snapshot,
                                                                graph::TopologySpec       topology,
                                                                const graph::NodeContext& context) {
    return graph::GraphCompiler::link_local(std::move(topology), context, snapshot.impl_->registry);
}

json RegistrySnapshot::manifest() const {
    return detail::owned_json_copy(impl_->manifest);
}

std::string RegistrySnapshot::serialize_canonical() const {
    return detail::canonical_json_bytes(impl_->manifest);
}

RegistrySnapshotBuilder::RegistrySnapshotBuilder() : impl_(std::make_unique<Impl>()) {}
RegistrySnapshotBuilder::RegistrySnapshotBuilder(RegistrySnapshotBuilder&&) noexcept = default;
RegistrySnapshotBuilder& RegistrySnapshotBuilder::operator=(RegistrySnapshotBuilder&&) noexcept =
    default;
RegistrySnapshotBuilder::~RegistrySnapshotBuilder() = default;

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_node(ExecutableManifest   manifest,
                                                           graph::NodeFactoryFn factory,
                                                           json                 config_schema,
                                                           json                 effects) {
    require_kind(manifest, ExecutableKind::Node);
    validate_manifest(manifest);
    if (!factory) throw std::invalid_argument("Node factory must not be empty");
    if (!config_schema.is_object() || !effects.is_object()) {
        throw std::invalid_argument("Node config_schema and effects must be objects");
    }
    config_schema = detail::owned_json_copy(config_schema);
    effects       = detail::owned_json_copy(effects);
    impl_->registry.register_type(manifest.identity.name, std::move(factory), config_schema,
                                  effects);
    impl_->entries.push_back(SnapshotEntry{
        std::move(manifest),
        detail::owned_json_copy(json{{"config_schema", config_schema}, {"effects", effects}})});
    return *this;
}

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_reducer(ExecutableManifest manifest,
                                                              graph::ReducerFn   reducer) {
    require_kind(manifest, ExecutableKind::Reducer);
    validate_manifest(manifest);
    if (!reducer) throw std::invalid_argument("Reducer callable must not be empty");
    impl_->registry.register_reducer(manifest.identity.name, std::move(reducer));
    impl_->entries.push_back(SnapshotEntry{std::move(manifest), json::object()});
    return *this;
}

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_condition(
    ExecutableManifest                  manifest,
    graph::ConditionFn                  condition,
    std::optional<graph::ConditionSpec> spec) {
    require_kind(manifest, ExecutableKind::Condition);
    validate_manifest(manifest);
    if (!condition) throw std::invalid_argument("Condition callable must not be empty");
    json metadata = json::object();
    if (spec) {
        normalize_strings(spec->labels, "condition labels");
        metadata["labels"] = spec->labels;
        metadata["open"]   = spec->open;
        impl_->registry.register_condition(manifest.identity.name, std::move(condition),
                                           std::move(*spec));
    } else {
        metadata["declared"] = false;
        impl_->registry.register_condition(manifest.identity.name, std::move(condition));
    }
    impl_->entries.push_back(SnapshotEntry{std::move(manifest), detail::owned_json_copy(metadata)});
    return *this;
}

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_provider(ExecutableManifest manifest,
                                                               ProviderMetadata   metadata) {
    require_kind(manifest, ExecutableKind::Provider);
    validate_manifest(manifest);
    if (!metadata.request_schema.is_object() || !metadata.response_schema.is_object()) {
        throw std::invalid_argument("Provider request_schema and response_schema must be objects");
    }
    impl_->entries.push_back(SnapshotEntry{
        std::move(manifest),
        detail::owned_json_copy(json{{"request_schema", metadata.request_schema},
                                     {"response_schema", metadata.response_schema}})});
    return *this;
}

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_tool(ExecutableManifest manifest,
                                                           ToolMetadata       metadata) {
    require_kind(manifest, ExecutableKind::Tool);
    validate_manifest(manifest);
    if (!metadata.input_schema.is_object() || !metadata.output_schema.is_object()) {
        throw std::invalid_argument("Tool input_schema and output_schema must be objects");
    }
    impl_->entries.push_back(
        SnapshotEntry{std::move(manifest),
                      detail::owned_json_copy(json{{"input_schema", metadata.input_schema},
                                                   {"output_schema", metadata.output_schema}})});
    return *this;
}

RegistrySnapshotBuilder& RegistrySnapshotBuilder::add_imported(
    ExecutableManifest manifest, ImportedExecutableMetadata metadata) {
    require_kind(manifest, ExecutableKind::Imported);
    validate_manifest(manifest);
    require_nonempty(metadata.module_id, "Imported module_id");
    require_nonempty(metadata.export_name, "Imported export_name");
    require_digest(metadata.dependency_digest, "Imported dependency_digest");
    validate_identity(metadata.target);
    if (metadata.target.kind == ExecutableKind::Imported) {
        throw std::invalid_argument("Imported executable target cannot itself be imported");
    }
    if (metadata.export_name != manifest.identity.name ||
        metadata.target.semantic_version != manifest.identity.semantic_version ||
        metadata.target.implementation_digest != manifest.identity.implementation_digest) {
        throw std::invalid_argument("Imported manifest identity does not match its exact target");
    }
    if (!manifest.required_executables.empty() &&
        (manifest.required_executables.size() != 1 ||
         manifest.required_executables.front() != metadata.target)) {
        throw std::invalid_argument("Imported executable may depend only on its exact target");
    }
    manifest.required_executables = {metadata.target};
    normalize_dependencies(manifest);
    impl_->imported_targets.push_back({manifest.identity.name, metadata.target});
    impl_->entries.push_back(SnapshotEntry{
        std::move(manifest),
        detail::owned_json_copy(json{{"module_id", metadata.module_id},
                                     {"export_name", metadata.export_name},
                                     {"dependency_digest", metadata.dependency_digest},
                                     {"target", encode_identity(metadata.target)}})});
    return *this;
}

RegistrySnapshot RegistrySnapshotBuilder::build() && {
    if (!impl_) throw std::logic_error("RegistrySnapshotBuilder was already consumed");
    std::sort(
        impl_->entries.begin(), impl_->entries.end(),
        [](const SnapshotEntry& lhs, const SnapshotEntry& rhs) {
            return std::tuple{to_string(lhs.manifest.identity.kind), lhs.manifest.identity.name} <
                   std::tuple{to_string(rhs.manifest.identity.kind), rhs.manifest.identity.name};
        });
    const auto duplicate =
        std::adjacent_find(impl_->entries.begin(), impl_->entries.end(),
                           [](const SnapshotEntry& lhs, const SnapshotEntry& rhs) {
                               return lhs.manifest.identity.kind == rhs.manifest.identity.kind &&
                                      lhs.manifest.identity.name == rhs.manifest.identity.name;
                           });
    if (duplicate != impl_->entries.end()) {
        throw std::invalid_argument("Duplicate executable registration: " +
                                    duplicate->manifest.identity.name);
    }
    for (const auto& binding : impl_->imported_targets) {
        const auto local = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                        [&binding](const SnapshotEntry& entry) {
                                            return entry.manifest.identity == binding.target;
                                        });
        if (local == impl_->entries.end()) {
            throw std::invalid_argument(
                "Imported executable target is absent from the RegistrySnapshot");
        }
        const auto alias = std::find_if(
            impl_->entries.begin(), impl_->entries.end(), [&binding](const SnapshotEntry& entry) {
                return entry.manifest.identity.kind == ExecutableKind::Imported &&
                       entry.manifest.identity.name == binding.alias;
            });
        if (alias == impl_->entries.end()) {
            throw std::logic_error("Imported executable alias is absent during snapshot sealing");
        }
        alias->manifest.effect_mode           = local->manifest.effect_mode;
        alias->manifest.required_capabilities = local->manifest.required_capabilities;
        alias->manifest.declared_effects      = local->manifest.declared_effects;
        validate_manifest(alias->manifest);
    }
    for (const auto& entry : impl_->entries) {
        for (const auto& dependency : entry.manifest.required_executables) {
            const auto local = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                            [&dependency](const SnapshotEntry& candidate) {
                                                return candidate.manifest.identity == dependency;
                                            });
            if (local == impl_->entries.end()) {
                throw std::invalid_argument(
                    "Required executable is absent or has a mismatched exact identity: " +
                    dependency.name);
            }
        }
    }

    json entries = json::array();
    for (const auto& entry : impl_->entries)
        entries.push_back(encode_manifest(entry));
    json       body{{"format", "neograph-registry-snapshot"},
                    {"storage_schema_version", kStorageSchemaVersion},
                    {"entries", std::move(entries)}};
    const auto fingerprint =
        detail::sha256_identity("registry-snapshot-v1", detail::canonical_json_bytes(body));
    body["fingerprint"] = fingerprint;

    auto result         = std::make_shared<RegistrySnapshot::Impl>();
    result->registry    = std::move(impl_->registry);
    result->entries     = std::move(impl_->entries);
    result->fingerprint = fingerprint;
    result->manifest    = detail::owned_json_copy(body);
    impl_.reset();
    return RegistrySnapshot(std::move(result));
}

std::string_view to_string(EffectMode mode) noexcept {
    switch (mode) {
        case EffectMode::Brokered:
            return "brokered";
        case EffectMode::TrustedNative:
            return "trusted_native";
    }
    return "unknown";
}

EffectMode effect_mode_from_string(std::string_view value) {
    if (value == "brokered") return EffectMode::Brokered;
    if (value == "trusted_native") return EffectMode::TrustedNative;
    throw std::invalid_argument("Unknown Program effect mode: " + std::string(value));
}

}  // namespace neograph::program
