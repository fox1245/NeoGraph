/**
 * @file program/bundle.h
 * @brief Immutable, content-addressed Program compilation artifacts.
 */
#pragma once

#include <neograph/program/coordinate.h>
#include <neograph/program/plan.h>
#include <neograph/program/source.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

/**
 * The lowest recoverability/integrity class reachable through an executable
 * closure. Strict is strongest; unmanaged is an explicit developer-authorized
 * escape hatch and must never be presented as strict recovery.
 */
enum class ExecutionGuarantee : std::uint8_t {
    Strict,
    Recorded,
    Unmanaged,
};

NEOGRAPH_PROGRAM_API std::string_view   to_string(ExecutionGuarantee guarantee) noexcept;
NEOGRAPH_PROGRAM_API ExecutionGuarantee execution_guarantee_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API std::uint8_t execution_guarantee_rank(ExecutionGuarantee guarantee) noexcept;

enum class ExecutableKind { Node, Reducer, Condition, Provider, Tool, Imported };

NEOGRAPH_PROGRAM_API std::string_view to_string(ExecutableKind kind) noexcept;
NEOGRAPH_PROGRAM_API ExecutableKind   executable_kind_from_string(std::string_view value);

struct ExecutableIdentity {
    ExecutableKind kind = ExecutableKind::Node;
    std::string    name;
    std::string    semantic_version;
    std::string    implementation_digest;

    bool operator==(const ExecutableIdentity&) const = default;
};

struct ContractRecord {
    std::uint32_t schema_version = 1;
    json          schema         = json::object();
};

/**
 * Validate the Program-v1 JSON Schema subset retained by a contract.
 *
 * Supported assertion keywords are type, const, enum, required, properties,
 * items, and additionalProperties. Other keywords are retained canonically as
 * annotations and do not affect runtime validation.
 */
NEOGRAPH_PROGRAM_API void validate_contract_schema(const ContractRecord& contract,
                                                   std::string_view      path = "$schema");
NEOGRAPH_PROGRAM_API void validate_contract_value(
    const json&           value,
    const ContractRecord& contract,
    std::string_view      subject = "Program contract value",
    std::string_view      path    = "$");

struct OrchestrationPlanRecord {
    std::uint32_t schema_version = 1;
    json          plan           = json::object();
};

struct SealedCoreDefinition {
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    std::string name;
    std::string definition_hash;
    json        definition = json::object();
};
/**
 * Returns the v1 content identity required by SealedCoreDefinition::definition_hash.
 */
NEOGRAPH_PROGRAM_API std::string sealed_core_definition_hash(const json& definition);
/**
 * Returns the deterministic Core compiled-plan identity for a sealed definition and the exact
 * executable closure resolved from one compiler and registry snapshot.
 */
NEOGRAPH_PROGRAM_API std::string core_compiled_plan_identity(
    const SealedCoreDefinition&            definition,
    std::string_view                       compiler_build_id,
    std::string_view                       registry_snapshot_fingerprint,
    const std::vector<ExecutableIdentity>& executable_closure);

struct CorePlanIdentity {
    std::string name;
    std::string compiled_plan_identity;

    bool operator==(const CorePlanIdentity&) const = default;
};

struct CapabilityEffectClosure {
    std::vector<std::string> capabilities;
    std::vector<std::string> effects;

    bool operator==(const CapabilityEffectClosure&) const = default;
};

struct BudgetRequirement {
    std::string   resource;
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;

    bool operator==(const BudgetRequirement&) const = default;
};

/**
 * Mutable construction input. ProgramBundle validates it, deep-copies JSON,
 * and canonicalizes semantic sets before deriving its identity:
 * definitions/plans by name, executables by
 * (kind,name,semantic_version,implementation_digest) with exactly one identity per (kind,name),
 * closure strings lexicographically, budgets by resource, and source maps by generated pointer.
 * Diagnostics retain producer order.
 */
struct ProgramBundleData {
    SourceKind source_kind =
        static_cast<SourceKind>(255);  ///< Required; invalid sentinel rejects omission.
    std::string source_hash;
    /**
     * Retained source for the opt-in JavaScript yielded-command controller.
     * Its canonical identity must exactly match source_kind and source_hash.
     */
    std::optional<ProgramSource> control_source;
    std::string                  canonical_program_hash;
    std::string                  compiler_build_id;
    std::uint32_t                program_schema_version = 1;
    std::string                  registry_snapshot_fingerprint;
    std::string                  module_dependency_merkle_root;
    /** Exact module coordinates represented by the dependency receipts. */
    std::vector<ModuleCoordinate>     module_coordinates;
    ContractRecord                    input_contract;
    ContractRecord                    output_contract;
    OrchestrationPlanRecord           orchestration_plan;
    std::vector<SealedCoreDefinition> sealed_core_definitions;
    std::vector<CorePlanIdentity>     core_plan_identities;
    CapabilityEffectClosure           capability_effect_closure;
    /// Exact weakest guarantee in the compiler-derived executable/control closure.
    ExecutionGuarantee              execution_guarantee = ExecutionGuarantee::Strict;
    std::vector<ExecutableIdentity> executable_registry_identities;
    std::vector<BudgetRequirement>  declared_budget_requirements;
    std::vector<SourceMapEntry>     source_map;
    std::vector<Diagnostic>         diagnostics;
};

class NEOGRAPH_PROGRAM_API ProgramBundle {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramBundle(ProgramBundleData data);
    static ProgramBundle parse(std::string_view stored_bytes);

    const std::string& id() const noexcept;
    SourceKind         source_kind() const noexcept;
    const std::string& source_hash() const noexcept;
    /** Retained JavaScript source when this bundle uses yielded control commands. */
    std::optional<ProgramSource>         control_source() const;
    const std::string&                   canonical_program_hash() const noexcept;
    const std::string&                   compiler_build_id() const noexcept;
    std::uint32_t                        program_schema_version() const noexcept;
    const std::string&                   registry_snapshot_fingerprint() const noexcept;
    const std::string&                   module_dependency_merkle_root() const noexcept;
    const std::vector<ModuleCoordinate>& module_coordinates() const noexcept;
    ContractRecord                       input_contract() const;
    ContractRecord                       output_contract() const;
    OrchestrationPlanRecord              orchestration_plan() const;
    /** Read-only typed view used by the direct Program scheduler. */
    const ProgramPlan&                     typed_orchestration_plan() const;
    std::vector<SealedCoreDefinition>      sealed_core_definitions() const;
    const std::vector<CorePlanIdentity>&   core_plan_identities() const noexcept;
    const CapabilityEffectClosure&         capability_effect_closure() const noexcept;
    ExecutionGuarantee                     execution_guarantee() const noexcept;
    const std::vector<ExecutableIdentity>& executable_registry_identities() const noexcept;
    const std::vector<BudgetRequirement>&  declared_budget_requirements() const noexcept;
    const std::vector<SourceMapEntry>&     source_map() const noexcept;
    std::vector<Diagnostic>                diagnostics() const;
    std::string                            serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramBundle(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
