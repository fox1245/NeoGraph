/**
 * @file program/module.h
 * @brief Immutable, owner-scoped Program modules and bounded child composition.
 */
#pragma once

#include <neograph/program/admission.h>
#include <neograph/program/coordinate.h>
#include <neograph/program/version.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class ModuleLifecycle : std::uint8_t {
    Active,
    Deprecated,
    Quarantined,
    Revoked,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ModuleLifecycle state) noexcept;
NEOGRAPH_PROGRAM_API ModuleLifecycle module_lifecycle_from_string(std::string_view value);

/** Typed child port. The contract is checked before a child can be dispatched. */
struct ModulePort {
    std::string    name;
    ContractRecord contract;

    bool operator==(const ModulePort&) const = default;
};

/** Child authority and budget are a strict attenuation of the parent module. */
struct ChildProgramDescriptor {
    std::string              name;
    std::string              program_version_id;
    std::vector<ModulePort>  inputs;
    std::vector<ModulePort>  outputs;
    std::vector<std::string> required_capabilities;
    std::vector<std::string> required_effects;
    BudgetLimits             budget;
    /**
     * Weakest child guarantee the parent explicitly accepts. A weaker child
     * rejects before dispatch; accepting degradation is immutable module data.
     */
    ExecutionGuarantee       minimum_execution_guarantee = ExecutionGuarantee::Strict;

    bool operator==(const ChildProgramDescriptor&) const = default;
};

struct ProgramModuleData {
    std::string                    owner_scope;
    ModuleCoordinate               coordinate;
    std::string                    attestation_id;
    ModuleLifecycle                lifecycle = ModuleLifecycle::Active;
    std::vector<ModuleCoordinate>  dependencies;
    /**
     * Optional reviewed pure JavaScript module body. When present it is part
     * of this immutable module's content identity and may be materialized
     * only through an exact dependency receipt.
     */
    std::optional<std::string>          pure_javascript_source;
    std::vector<ChildProgramDescriptor> children;
    std::vector<std::string>       allowed_capabilities;
    std::vector<std::string>       declared_effects;
};

/**
 * Immutable module metadata. Lifecycle state is operational metadata and is
 * deliberately excluded from the content identity, so revocation cannot
 * manufacture a new dependency identity.
 */
class NEOGRAPH_PROGRAM_API ProgramModule final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ProgramModule create(ProgramModuleData data);
    static ProgramModule parse(std::string_view stored_bytes);

    const std::string& owner_scope() const noexcept;
    const ModuleCoordinate& coordinate() const noexcept;
    const std::string& attestation_id() const noexcept;
    ModuleLifecycle lifecycle() const noexcept;
    const std::vector<ModuleCoordinate>& dependencies() const noexcept;
    const std::vector<ChildProgramDescriptor>& children() const noexcept;
    const std::optional<std::string>& pure_javascript_source() const noexcept;
    const std::vector<std::string>& allowed_capabilities() const noexcept;
    const std::vector<std::string>& declared_effects() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramModule(std::shared_ptr<const Impl> impl);
    static ProgramModule with_lifecycle(const ProgramModule& module, ModuleLifecycle lifecycle);

    std::shared_ptr<const Impl> impl_;
    friend class InMemoryModuleStore;
};

/** Result of resolving one module and its complete pinned dependency closure. */
struct ModuleResolution {
    ModuleCoordinate              root;
    std::vector<ProgramModule>    modules;
    std::vector<DependencyReceipt> receipts;

    std::string dependency_merkle_root() const;
    std::vector<ImportRef> imports() const;
    std::vector<ModuleCoordinate> coordinates() const;
};

/**
 * Immutable allowlist view over a verified ModuleResolution.  JavaScript
 * module lookup is by the exact receipt source id; there is no range,
 * filesystem, process-global, or network fallback.
 */
class NEOGRAPH_PROGRAM_API VerifiedModuleResolver final {
public:
    explicit VerifiedModuleResolver(ModuleResolution resolution);

    VerifiedModuleResolver(VerifiedModuleResolver&&) noexcept;
    VerifiedModuleResolver& operator=(VerifiedModuleResolver&&) noexcept;
    VerifiedModuleResolver(const VerifiedModuleResolver&)            = delete;
    VerifiedModuleResolver& operator=(const VerifiedModuleResolver&) = delete;
    ~VerifiedModuleResolver();

    const ModuleResolution& resolution() const noexcept;
    std::optional<ModuleCoordinate> resolve(std::string_view source_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/** A child version and its immutable bundle, paired before dispatch. */
struct ChildProgramBinding {
    std::string   child_name;
    ProgramBundle bundle;
    ProgramVersion version;
};

/** Complete pre-dispatch composition proof for one parent Program. */
struct ProgramComposition {
    ProgramModule parent;
    ModuleResolution resolution;
    std::vector<ChildProgramBinding> children;
};

/**
 * Validate the whole composed Program before any GraphEngine materialization.
 * This checks the module closure, all typed ports, exact child versions,
 * authority/effect attenuation, bounded child budgets, and duplicate names or
 * identities. It does not execute a child or create another executor.
 */
NEOGRAPH_PROGRAM_API void validate_program_composition(
    const ProgramBundle& parent_bundle, const ProgramComposition& composition);

/** Validate a resolver-produced closure, including its pinned graph and receipts. */
NEOGRAPH_PROGRAM_API void validate_module_resolution(const ModuleResolution& resolution);

/**
 * Immutable proof that one child descriptor was linked to one exact admitted
 * Program version. The receipt is the only module metadata accepted by the
 * child runtime; callers cannot substitute a version after linking.
 */
struct ModuleLinkReceiptData {
    std::string             owner_scope;
    std::string             parent_module_id;
    std::string             dependency_merkle_root;
    std::string             child_name;
    std::string             child_program_version_id;
    std::string             child_bundle_id;
    std::string             child_input_contract_fingerprint;
    std::string             child_output_contract_fingerprint;
    std::vector<std::string> granted_capabilities;
    std::vector<std::string> granted_effects;
    BudgetLimits             budget;
    ExecutionGuarantee       minimum_execution_guarantee = ExecutionGuarantee::Strict;
};

class NEOGRAPH_PROGRAM_API ModuleLinkReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ModuleLinkReceipt create(ModuleLinkReceiptData data);
    static ModuleLinkReceipt parse(std::string_view stored_bytes);

    const std::string& owner_scope() const noexcept;
    const std::string& parent_module_id() const noexcept;
    const std::string& dependency_merkle_root() const noexcept;
    const std::string& child_name() const noexcept;
    const std::string& child_program_version_id() const noexcept;
    const std::string& child_bundle_id() const noexcept;
    const std::string& child_input_contract_fingerprint() const noexcept;
    const std::string& child_output_contract_fingerprint() const noexcept;
    const std::vector<std::string>& granted_capabilities() const noexcept;
    const std::vector<std::string>& granted_effects() const noexcept;
    const BudgetLimits& budget() const noexcept;
    ExecutionGuarantee minimum_execution_guarantee() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ModuleLinkReceipt(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> impl_;
};

/** Runtime-only immutable child binding supplied by an owning resolver. */
struct ProgramRuntimeChildBinding {
    ModuleLinkReceipt receipt;
    ProgramVersion    version;
};

using ProgramChildBindingResolver = std::function<std::optional<ProgramRuntimeChildBinding>(
    std::string_view owner_scope,
    std::string_view parent_program_version_id,
    std::string_view child_binding_name)>;

/**
 * Link one child descriptor from a verified module closure to an exact
 * admitted ProgramVersion. The operation is pure and fails closed on owner,
 * lifecycle, port, capability, effect, identity, or budget mismatches.
 */
NEOGRAPH_PROGRAM_API ModuleLinkReceipt link_module_child(
    const ModuleResolution& resolution,
    const ProgramModule&    parent,
    std::string_view        child_name,
    const ProgramBundle&    child_bundle,
    const ProgramVersion&   child);

class NEOGRAPH_PROGRAM_API ModuleStore {
public:
    virtual ~ModuleStore() = default;
    virtual void publish(const ProgramModule& module) = 0;
    virtual std::optional<ProgramModule> get(std::string_view content_identity) const = 0;
    /** Wrong-owner module lookups are indistinguishable from absence. */
    virtual std::optional<ProgramModule> get(std::string_view owner_scope,
                                             std::string_view content_identity) const {
        if (owner_scope.empty()) return std::nullopt;
        const auto module = get(content_identity);
        if (!module || module->owner_scope() != owner_scope) return std::nullopt;
        return module;
    }
    virtual void set_lifecycle(std::string_view content_identity, ModuleLifecycle lifecycle) = 0;
};

/** Small durable-boundary-compatible store used by the in-process resolver. */
class NEOGRAPH_PROGRAM_API InMemoryModuleStore final : public ModuleStore {
public:
    InMemoryModuleStore();
    InMemoryModuleStore(InMemoryModuleStore&&) noexcept;
    InMemoryModuleStore& operator=(InMemoryModuleStore&&) noexcept;
    InMemoryModuleStore(const InMemoryModuleStore&) = delete;
    InMemoryModuleStore& operator=(const InMemoryModuleStore&) = delete;
    ~InMemoryModuleStore() override;

    void publish(const ProgramModule& module) override;
    std::optional<ProgramModule> get(std::string_view content_identity) const override;
    std::optional<ProgramModule> get(std::string_view owner_scope,
                                     std::string_view content_identity) const override;
    void set_lifecycle(std::string_view content_identity, ModuleLifecycle lifecycle) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Resolves exact, pinned modules. Missing, cyclic, owner-mismatched,
 * unapproved, quarantined, and revoked dependencies fail closed.
 */
class NEOGRAPH_PROGRAM_API ModuleResolver final {
public:
    explicit ModuleResolver(std::shared_ptr<const ModuleStore> store);

    ModuleResolution resolve(const ModuleCoordinate& root,
                             const PolicySnapshot& policy) const;

private:
    std::shared_ptr<const ModuleStore> store_;
};

}  // namespace neograph::program
