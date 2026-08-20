/** @file program/synthesis.h @brief Host-owned bounded Program synthesis gateway. */
#pragma once

#include <neograph/program/catalog.h>
#include <neograph/program/compiler.h>
#include <neograph/program/result.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

struct ProgramSynthesisProposalData {
    std::string owner_scope;
    std::string lineage_id;
    std::string parent_run_id;
    std::optional<std::string> instruction_id;
    std::optional<ProgramSource> source;
    std::vector<std::string> requested_capabilities;
    std::vector<std::string> requested_effects;
    RunBudget requested_budget;
    std::int64_t created_at_ms = 0;
};

/** Immutable non-executable request to compile one sealed JavaScript successor. */
class NEOGRAPH_PROGRAM_API ProgramSynthesisProposal final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    static ProgramSynthesisProposal create(ProgramSynthesisProposalData data);
    static ProgramSynthesisProposal parse(std::string_view stored_bytes);

    const ProgramSynthesisProposalData& data() const noexcept;
    const ProgramSource& source() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramSynthesisProposal(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramSynthesisReservationData {
    std::string proposal_id;
    std::string lineage_id;
    std::string source_lineage_head_id;
    std::string reserved_lineage_head_id;
    RunBudget source_remaining;
    RunBudget remaining_after_reservation;
};

/** Host evidence that one nonrenewable dynamic-compile unit was debited. */
class NEOGRAPH_PROGRAM_API ProgramSynthesisReservation final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    static ProgramSynthesisReservation create(ProgramSynthesisReservationData data);
    static ProgramSynthesisReservation parse(std::string_view stored_bytes);

    const ProgramSynthesisReservationData& data() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramSynthesisReservation(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramSynthesisReceiptData {
    std::string proposal_id;
    std::string reservation_id;
    std::string bundle_id;
    std::string program_version_id;
    std::string policy_snapshot_fingerprint;
};

class NEOGRAPH_PROGRAM_API ProgramSynthesisReceipt final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;
    static ProgramSynthesisReceipt create(ProgramSynthesisReceiptData data);
    static ProgramSynthesisReceipt parse(std::string_view stored_bytes);

    const ProgramSynthesisReceiptData& data() const noexcept;
    const std::string& id() const noexcept;
    std::string serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramSynthesisReceipt(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

using ProgramSynthesisReservationResolver =
    std::function<ProgramSynthesisReservation(const ProgramSynthesisProposal&)>;
using ProgramSynthesisAdmissionResolver =
    std::function<ProgramAdmission(const ProgramSynthesisProposal&,
                                   const ProgramBundle&,
                                   const ProgramSynthesisReservation&)>;

struct ProgramSynthesisGatewayConfig {
    std::shared_ptr<ProgramCompiler> compiler;
    std::shared_ptr<ProgramCatalog> catalog;
    ProgramSynthesisReservationResolver reserve;
    ProgramSynthesisAdmissionResolver admission;
    std::size_t max_source_bytes = 1024 * 1024;
};

struct ProgramSynthesisResult {
    ProgramSynthesisReservation reservation;
    ProgramBundle bundle;
    ProgramVersion version;
    ProgramSynthesisReceipt receipt;
};

/** Enforces reserve -> compile -> admit; it never activates, binds, or dispatches. */
class NEOGRAPH_PROGRAM_API ProgramSynthesisGateway final {
public:
    explicit ProgramSynthesisGateway(ProgramSynthesisGatewayConfig config);
    ProgramSynthesisResult synthesize(const ProgramSynthesisProposal& proposal) const;
private:
    ProgramSynthesisGatewayConfig config_;
};

}  // namespace neograph::program
