/**
 * @file program/version.h
 * @brief Immutable stored binding between a Program bundle and admission receipts.
 */
#pragma once

#include <neograph/program/admission.h>
#include <neograph/program/bundle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program {

struct DependencyReceipt {
    std::string dependency_id;
    std::string content_identity;

    bool operator==(const DependencyReceipt&) const = default;
};

struct CapabilityBindingReceipt {
    ExecutableIdentity executable;
    std::string        binding_identity;

    bool operator==(const CapabilityBindingReceipt&) const = default;
};

NEOGRAPH_PROGRAM_API std::string capability_binding_receipt_root(
    std::vector<CapabilityBindingReceipt> receipts);

struct CoreMaterializationReceipt {
    std::string compiler_build_id;
    std::string registry_snapshot_fingerprint;
    /// Canonicalized by plan name.
    std::vector<CorePlanIdentity> plans;
    /// Canonicalized by exact executable identity.
    std::vector<CapabilityBindingReceipt> capability_bindings;

    bool operator==(const CoreMaterializationReceipt&) const = default;
};

/**
 * Mutable construction input. ProgramVersion sorts dependency receipts by
 * dependency_id; materialization plans use their documented name ordering.
 */
struct ProgramVersionData {
    ProgramVersionData(std::string                    bundle,
                       AdmissionProfile               admission,
                       PolicySnapshot                 policy,
                       std::vector<DependencyReceipt> dependencies,
                       std::string                    owner,
                       CoreMaterializationReceipt     materialization)
        : bundle_id(std::move(bundle)),
          admission_profile(std::move(admission)),
          policy_snapshot(std::move(policy)),
          dependency_receipts(std::move(dependencies)),
          ownership_scope(std::move(owner)),
          core_materialization_receipt(std::move(materialization)) {}

    std::string                    bundle_id;
    AdmissionProfile               admission_profile;
    PolicySnapshot                 policy_snapshot;
    std::vector<DependencyReceipt> dependency_receipts;
    std::string                    ownership_scope;
    CoreMaterializationReceipt     core_materialization_receipt;
};

class NEOGRAPH_PROGRAM_API ProgramVersion {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramVersion(ProgramVersionData data);
    static ProgramVersion parse(std::string_view stored_bytes);

    const std::string&                    id() const noexcept;
    const std::string&                    bundle_id() const noexcept;
    AdmissionProfile                      admission_profile() const;
    PolicySnapshot                        policy_snapshot() const;
    const std::vector<DependencyReceipt>& dependency_receipts() const noexcept;
    const std::string&                    ownership_scope() const noexcept;
    const CoreMaterializationReceipt&     core_materialization_receipt() const noexcept;
    std::string                           serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramVersion(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

}  // namespace neograph::program
