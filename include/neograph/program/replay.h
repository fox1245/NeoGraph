/**
 * @file program/replay.h
 * @brief Protocol-neutral, exact-bound recorded capability replay values.
 */
#pragma once

#include <neograph/program/catalog.h>
#include <neograph/program/pending.h>
#include <neograph/program/transition_store.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class RecordedEvidenceCoverage : std::uint8_t { Full, Partial, Redacted };

NEOGRAPH_PROGRAM_API std::string_view to_string(RecordedEvidenceCoverage coverage) noexcept;
NEOGRAPH_PROGRAM_API RecordedEvidenceCoverage
recorded_evidence_coverage_from_string(std::string_view value);

struct RecordedCapabilityFailure {
    std::string code;
    std::string classification;
    std::string message;
    json        witness = json::object();

    bool operator==(const RecordedCapabilityFailure&) const = default;
};

/** Exact expected coordinate loaded from the source run's durable call/effect ledger. */
struct RecordedCapabilityCallReference {
    std::uint64_t              sequence = 0;
    CapabilityBindingReceipt  binding;
    std::string                operation_id;
    std::string                call_id;
    std::optional<std::string> effect_id;

    bool operator==(const RecordedCapabilityCallReference&) const = default;
};

struct RecordedCapabilityEvidenceData {
    RecordedCapabilityCallReference          reference;
    RecordedEvidenceCoverage                 coverage = RecordedEvidenceCoverage::Partial;
    bool                                     redacted = false;
    std::optional<ProgramPendingInput>        input_outcome;
    std::optional<ProgramPendingEffect>       effect_outcome;
    std::optional<RecordedCapabilityFailure> failure;
};

/** Canonical immutable result or failure for one exact recorded capability call. */
class NEOGRAPH_PROGRAM_API RecordedCapabilityEvidence {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit RecordedCapabilityEvidence(RecordedCapabilityEvidenceData data);
    static RecordedCapabilityEvidence parse(std::string_view stored_bytes);

    const std::string&                         id() const noexcept;
    const RecordedCapabilityCallReference&    reference() const noexcept;
    RecordedEvidenceCoverage                  coverage() const noexcept;
    bool                                      redacted() const noexcept;
    std::optional<ProgramPendingInput>         input_outcome() const;
    std::optional<ProgramPendingEffect>        effect_outcome() const;
    std::optional<RecordedCapabilityFailure>  failure() const;
    std::string                               serialize_canonical() const;

private:
    struct Impl;
    explicit RecordedCapabilityEvidence(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/**
 * Move-only owned replay binding. Construction validates, before Runtime can
 * create a target run, that:
 * - the owned binding has exactly the pinned ProgramVersion receipts;
 * - every expected call/effect coordinate appears once and in source order;
 * - all evidence is FULL, non-redacted, exact-bound, and has one result/failure.
 *
 * The contained CatalogCapabilityBinding is supplied directly to Catalog's
 * explicit-owned materialization path; the configured live binder is never
 * consulted and there is no live fallback.
 */
class NEOGRAPH_PROGRAM_API RecordedBindingSet {
public:
    RecordedBindingSet(std::vector<CapabilityBindingReceipt>        exact_bindings,
                       std::vector<RecordedCapabilityCallReference> expected_calls,
                       CatalogCapabilityBinding                     owned_binding,
                       std::vector<RecordedCapabilityEvidence>      evidence);
    RecordedBindingSet(RecordedBindingSet&&) noexcept;
    RecordedBindingSet& operator=(RecordedBindingSet&&) noexcept;
    RecordedBindingSet(const RecordedBindingSet&)            = delete;
    RecordedBindingSet& operator=(const RecordedBindingSet&) = delete;
    ~RecordedBindingSet();

    const std::string&                                  fingerprint() const noexcept;
    const std::vector<CapabilityBindingReceipt>&        exact_bindings() const noexcept;
    const std::vector<RecordedCapabilityCallReference>& expected_calls() const noexcept;
    const std::vector<RecordedCapabilityEvidence>&      evidence() const noexcept;

    /** Re-check against the pinned target version immediately before start. */
    void validate_target(const ProgramVersion& target) const;

    /** One-shot transfer into Catalog::resolve_version_with_binding. */
    CatalogCapabilityBinding release_owned_binding() &&;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ProgramHistoricalReplacementChain;

NEOGRAPH_PROGRAM_API ProgramHistoricalReplacementChain
inspect_program_replacement_chain(const ProgramTransitionStore& store,
                                  std::string_view owner_scope,
                                  std::string_view any_run_id);

/** One fully revalidated transition in a historical replacement chain. */
class NEOGRAPH_PROGRAM_API ProgramHistoricalReplacementStep final {
public:
    const ProgramRunGeneration& source_generation() const noexcept;
    const ProgramRunLineage& source_lineage() const noexcept;
    const ProgramRunRecord& source_run() const noexcept;
    const ProgramJavaScriptCommandJournalEntry& checkpoint() const noexcept;
    const ProgramRunGeneration& target_generation() const noexcept;
    const ProgramRunLineage& target_initial_lineage() const noexcept;
    const ProgramTransitionPublication& target_initial_publication() const noexcept;

private:
    struct Impl;
    explicit ProgramHistoricalReplacementStep(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;

    friend ProgramHistoricalReplacementChain inspect_program_replacement_chain(
        const ProgramTransitionStore&, std::string_view, std::string_view);
};

/** Read-only, fail-closed evidence for one immutable A -> ... -> N lineage. */
class NEOGRAPH_PROGRAM_API ProgramHistoricalReplacementChain final {
public:
    const ProgramRunLineage& anchor() const noexcept;
    const std::vector<ProgramRunGeneration>& generations() const noexcept;
    const std::vector<ProgramHistoricalReplacementStep>& replacements() const noexcept;

private:
    struct Impl;
    explicit ProgramHistoricalReplacementChain(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;

    friend ProgramHistoricalReplacementChain inspect_program_replacement_chain(
        const ProgramTransitionStore&, std::string_view, std::string_view);
};

}  // namespace neograph::program
