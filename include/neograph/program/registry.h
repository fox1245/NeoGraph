/**
 * @file program/registry.h
 * @brief Immutable executable registry snapshots for Program admission.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/registry.h>
#include <neograph/json.h>
#include <neograph/program/bundle.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

namespace detail {
class RegistrySnapshotAccess;
}

enum class EffectMode : std::uint8_t {
    Brokered,
    TrustedNative,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(EffectMode mode) noexcept;
NEOGRAPH_PROGRAM_API EffectMode       effect_mode_from_string(std::string_view value);

struct ExecutableManifest {
    ExecutableIdentity       identity;
    EffectMode               effect_mode = EffectMode::Brokered;
    std::string              attestation_id;  // host declaration id; not a signature
    std::vector<std::string> required_capabilities;
    std::vector<std::string> declared_effects;
};

struct ProviderMetadata {
    json request_schema;
    json response_schema;
};

struct ToolMetadata {
    json input_schema;
    json output_schema;
};

struct ImportedExecutableMetadata {
    std::string        module_id;
    std::string        export_name;
    std::string        dependency_digest;
    ExecutableIdentity target;
};

class NEOGRAPH_PROGRAM_API RegistrySnapshot {
public:
    RegistrySnapshot(const RegistrySnapshot&) noexcept            = default;
    RegistrySnapshot(RegistrySnapshot&&) noexcept                 = default;
    RegistrySnapshot& operator=(const RegistrySnapshot&) noexcept = default;
    RegistrySnapshot& operator=(RegistrySnapshot&&) noexcept      = default;
    ~RegistrySnapshot();

    const std::string&                fingerprint() const noexcept;
    std::vector<ExecutableIdentity>   identities() const;
    std::optional<ExecutableManifest> find(ExecutableKind kind, std::string_view name) const;
    json                              manifest() const;
    std::string                       serialize_canonical() const;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit RegistrySnapshot(std::shared_ptr<const Impl> impl);
    friend class RegistrySnapshotBuilder;
    friend class detail::RegistrySnapshotAccess;
};

/**
 * @brief Move-only builder for one sealed, process-local executable registry.
 *
 * Node, reducer, and condition callables are copied into registry-owned
 * std::function wrappers. Their capture graphs remain trusted embedding state:
 * callers must keep captures valid and thread-safe, and any semantic change
 * requires a new implementation digest. Provider and tool entries contain
 * metadata only; no runtime instance, transport client, or borrowed registry is
 * retained.
 */
class NEOGRAPH_PROGRAM_API RegistrySnapshotBuilder {
public:
    RegistrySnapshotBuilder();
    RegistrySnapshotBuilder(RegistrySnapshotBuilder&&) noexcept;
    RegistrySnapshotBuilder& operator=(RegistrySnapshotBuilder&&) noexcept;
    RegistrySnapshotBuilder(const RegistrySnapshotBuilder&)            = delete;
    RegistrySnapshotBuilder& operator=(const RegistrySnapshotBuilder&) = delete;
    ~RegistrySnapshotBuilder();

    RegistrySnapshotBuilder& add_node(ExecutableManifest   manifest,
                                      graph::NodeFactoryFn factory,
                                      json                 config_schema,
                                      json                 effects);
    RegistrySnapshotBuilder& add_reducer(ExecutableManifest manifest, graph::ReducerFn reducer);
    RegistrySnapshotBuilder& add_condition(ExecutableManifest                  manifest,
                                           graph::ConditionFn                  condition,
                                           std::optional<graph::ConditionSpec> spec);
    RegistrySnapshotBuilder& add_provider(ExecutableManifest manifest, ProviderMetadata metadata);
    RegistrySnapshotBuilder& add_tool(ExecutableManifest manifest, ToolMetadata metadata);
    RegistrySnapshotBuilder& add_imported(ExecutableManifest         manifest,
                                          ImportedExecutableMetadata metadata);

    RegistrySnapshot build() &&;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
