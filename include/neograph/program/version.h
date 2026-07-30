/**
 * @file program/version.h
 * @brief Immutable stored binding between a Program bundle and admission receipts.
 */
#pragma once

#include <neograph/program/bundle.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

struct AdmissionProfileBinding {
    std::string profile_id;
    std::string profile_fingerprint;

    bool operator==(const AdmissionProfileBinding&) const = default;
};

struct PolicySnapshotBinding {
    std::string snapshot_id;
    std::string snapshot_fingerprint;

    bool operator==(const PolicySnapshotBinding&) const = default;
};

struct DependencyReceipt {
    std::string dependency_id;
    std::string content_identity;

    bool operator==(const DependencyReceipt&) const = default;
};

struct CoreMaterializationReceipt {
    std::string compiler_build_id;
    std::string registry_snapshot_fingerprint;
    /// Canonicalized by plan name.
    std::vector<CorePlanIdentity> plans;

    bool operator==(const CoreMaterializationReceipt&) const = default;
};

/**
 * Mutable construction input. ProgramVersion sorts dependency receipts by
 * dependency_id; materialization plans use their documented name ordering.
 */
struct ProgramVersionData {
    std::string                    bundle_id;
    AdmissionProfileBinding        admission_profile;
    PolicySnapshotBinding          policy_snapshot;
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
    const AdmissionProfileBinding&        admission_profile() const noexcept;
    const PolicySnapshotBinding&          policy_snapshot() const noexcept;
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
