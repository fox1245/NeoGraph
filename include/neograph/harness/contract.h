/**
 * @file harness/contract.h
 * @brief Contract-driven Harness manifest and evidence boundary.
 *
 * Harness deliberately delegates lifecycle and verification to Program's
 * durable value types.  This facade adds no executor or transport; it is the
 * small API used by Harness callers to gate worker selection and publication.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/contract.h>

#include <string_view>

namespace neograph::harness {

using ManifestLifecycle = program::ContractManifestLifecycle;
using ManifestSpec = program::ContractManifestSpec;
using ManifestReview = program::ContractReview;
using Manifest = program::ContractManifest;
using EvidenceKind = program::ContractEvidenceKind;
using Evidence = program::ContractEvidence;
using Diagnostic = program::ContractDiagnostic;
using RunStatus = program::ContractRunStatus;
using Verification = program::ContractVerification;
using Run = program::ContractRun;

/**
 * Program-owned contract boundary for Harness.
 *
 * Every operation returns or mutates the existing Program contract values;
 * no worker, provider, or transport is executed here.  In particular,
 * start_run rejects a proposal/review and record_evidence rejects worker
 * self-reports, leaving publication to independent verification.
 */
class NEOGRAPH_HARNESS_API ContractBoundary final {
public:
    static Manifest propose(ManifestSpec spec);
    static Manifest review(const Manifest& manifest, ManifestReview review);
    static Manifest freeze(const Manifest& manifest);

    static bool worker_selection_allowed(const Manifest& manifest) noexcept;
    static Run start_run(const Manifest& manifest);

    static void record_evidence(Run& run, Evidence evidence);
    static void record_diagnostic(Run& run, Diagnostic diagnostic);
    static Verification verify(Run& run,
                               std::string_view program_version_id,
                               std::string_view workspace_revision);
    static void publish(Run& run);
};

}  // namespace neograph::harness
