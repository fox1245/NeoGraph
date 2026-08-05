/**
 * @file program/admission.h
 * @brief Immutable Program admission and policy snapshots.
 */
#pragma once

#include <neograph/program/registry.h>
#include <neograph/program/source.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

inline constexpr std::string_view TRUSTED_NATIVE_CAPABILITY = "neograph.native.trusted";
/// Default grant for a caller that explicitly opts into bounded child execution.
inline constexpr std::uint32_t DEFAULT_MAX_CHILD_DEPTH = 1;
/// Hard upper bound for recursive child grants in one Program lineage.
inline constexpr std::uint32_t MAX_SUPPORTED_CHILD_DEPTH = 8;

enum class AdmissionMode : std::uint8_t {
    MultiTenant,
    TrustedEmbedding,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(AdmissionMode mode) noexcept;
NEOGRAPH_PROGRAM_API AdmissionMode    admission_mode_from_string(std::string_view value);

struct BudgetLimits {
    std::uint64_t wall_time_ms           = 0;
    std::uint64_t model_tokens           = 0;
    std::uint64_t monetary_microunits    = 0;
    std::uint32_t max_concurrency        = 0;
    std::uint64_t max_program_operations = 0;
    std::uint64_t max_core_steps         = 0;
    std::uint32_t max_dynamic_compiles   = 0;
    std::uint32_t max_child_depth        = 0;
    std::uint64_t max_total_children     = 0;

    bool operator==(const BudgetLimits&) const = default;
};

class NEOGRAPH_PROGRAM_API AdmissionProfile {
public:
    AdmissionProfile(const AdmissionProfile&) noexcept            = default;
    AdmissionProfile(AdmissionProfile&&) noexcept                 = default;
    AdmissionProfile& operator=(const AdmissionProfile&) noexcept = default;
    AdmissionProfile& operator=(AdmissionProfile&&) noexcept      = default;
    ~AdmissionProfile();

    const std::string&              id() const noexcept;
    const std::string&              semantic_version() const noexcept;
    const std::string&              fingerprint() const noexcept;
    const std::string&              registry_fingerprint() const noexcept;
    AdmissionMode                   mode() const noexcept;
    std::uint32_t                   max_program_schema_version() const noexcept;
    std::vector<SourceKind>         allowed_source_kinds() const;
    std::vector<ExecutableIdentity> allowed_executables() const;
    std::vector<EffectMode>         allowed_effect_modes() const;
    json                            manifest() const;
    std::string                     serialize_canonical() const;

    static AdmissionProfile parse(std::string_view bytes);

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit AdmissionProfile(std::shared_ptr<const Impl> impl);
    friend class AdmissionProfileBuilder;
    friend class PolicySnapshotBuilder;
};

class NEOGRAPH_PROGRAM_API AdmissionProfileBuilder {
public:
    AdmissionProfileBuilder();
    AdmissionProfileBuilder(AdmissionProfileBuilder&&) noexcept;
    AdmissionProfileBuilder& operator=(AdmissionProfileBuilder&&) noexcept;
    AdmissionProfileBuilder(const AdmissionProfileBuilder&)            = delete;
    AdmissionProfileBuilder& operator=(const AdmissionProfileBuilder&) = delete;
    ~AdmissionProfileBuilder();

    AdmissionProfileBuilder& id(std::string value);
    AdmissionProfileBuilder& semantic_version(std::string value);
    AdmissionProfileBuilder& registry(RegistrySnapshot snapshot);
    AdmissionProfileBuilder& mode(AdmissionMode value);
    AdmissionProfileBuilder& max_program_schema_version(std::uint32_t value);
    AdmissionProfileBuilder& allow_source_kind(SourceKind value);
    AdmissionProfileBuilder& allow_executable(ExecutableIdentity value);
    AdmissionProfileBuilder& allow_effect_mode(EffectMode value);

    AdmissionProfile build() &&;

private:
    struct Impl;
    friend class AdmissionProfile;
    std::unique_ptr<Impl> impl_;
};

class NEOGRAPH_PROGRAM_API PolicySnapshot {
public:
    PolicySnapshot(const PolicySnapshot&) noexcept            = default;
    PolicySnapshot(PolicySnapshot&&) noexcept                 = default;
    PolicySnapshot& operator=(const PolicySnapshot&) noexcept = default;
    PolicySnapshot& operator=(PolicySnapshot&&) noexcept      = default;
    ~PolicySnapshot();

    const std::string&       id() const noexcept;
    const std::string&       semantic_version() const noexcept;
    const std::string&       owner_scope() const noexcept;
    const std::string&       fingerprint() const noexcept;
    const std::string&       admission_profile_fingerprint() const noexcept;
    const std::string&       registry_fingerprint() const noexcept;
    std::vector<std::string> allowed_capabilities() const;
    std::vector<std::string> allowed_effects() const;
    std::vector<std::string> allowed_module_digests() const;
    const BudgetLimits&      budget_ceiling() const noexcept;
    json                     manifest() const;
    std::string              serialize_canonical() const;

    static PolicySnapshot parse(std::string_view bytes);

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;

    explicit PolicySnapshot(std::shared_ptr<const Impl> impl);
    friend class PolicySnapshotBuilder;
};

class NEOGRAPH_PROGRAM_API PolicySnapshotBuilder {
public:
    PolicySnapshotBuilder();
    PolicySnapshotBuilder(PolicySnapshotBuilder&&) noexcept;
    PolicySnapshotBuilder& operator=(PolicySnapshotBuilder&&) noexcept;
    PolicySnapshotBuilder(const PolicySnapshotBuilder&)            = delete;
    PolicySnapshotBuilder& operator=(const PolicySnapshotBuilder&) = delete;
    ~PolicySnapshotBuilder();

    PolicySnapshotBuilder& id(std::string value);
    PolicySnapshotBuilder& semantic_version(std::string value);
    PolicySnapshotBuilder& owner_scope(std::string value);
    PolicySnapshotBuilder& admission_profile(AdmissionProfile value);
    PolicySnapshotBuilder& allow_capability(std::string value);
    PolicySnapshotBuilder& allow_effect(std::string value);
    PolicySnapshotBuilder& allow_module_digest(std::string value);
    PolicySnapshotBuilder& budget_ceiling(BudgetLimits value);

    PolicySnapshot build() &&;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace neograph::program
