#include <neograph/harness/contract.h>

#include <algorithm>
#include <stdexcept>
#include <utility>


namespace neograph::harness {
namespace {
void validate_harness_spec(const ManifestSpec& spec) {
    if (spec.assumptions.empty() || spec.requirements.empty() || spec.non_goals.empty() ||
        spec.acceptance.empty() || spec.fixed_test_vectors.empty() ||
        spec.independent_oracles.empty() || spec.risk_register.empty()) {
        throw std::invalid_argument(
            "Harness contract requires assumptions, requirements, non-goals, acceptance, "
            "test vectors, independent oracles, and a risk register");
    }
    if (std::none_of(spec.acceptance.begin(), spec.acceptance.end(),
                     [](const auto& item) { return item.required; })) {
        throw std::invalid_argument("Harness contract requires a required acceptance gate");
    }
}
}  // namespace

Manifest ContractBoundary::propose(ManifestSpec spec) {
    validate_harness_spec(spec);
    return Manifest::propose(std::move(spec));
}

Manifest ContractBoundary::review(const Manifest& manifest, ManifestReview review_record) {
    validate_harness_spec(manifest.spec());
    return manifest.review(std::move(review_record));
}

Manifest ContractBoundary::freeze(const Manifest& manifest) {
    validate_harness_spec(manifest.spec());
    return manifest.freeze();
}

bool ContractBoundary::worker_selection_allowed(const Manifest& manifest) noexcept {
    return manifest.lifecycle() == ManifestLifecycle::Frozen;
}

Run ContractBoundary::start_run(const Manifest& manifest) {
    validate_harness_spec(manifest.spec());
    if (!worker_selection_allowed(manifest)) {
        throw std::invalid_argument("Harness worker selection requires a frozen manifest");
    }
    return Run(manifest);
}

void ContractBoundary::record_evidence(Run& run, Evidence evidence) {
    run.record_evidence(std::move(evidence));
}

void ContractBoundary::record_diagnostic(Run& run, Diagnostic diagnostic) {
    run.record_diagnostic(std::move(diagnostic));
}
Verification ContractBoundary::verify(Run& run,
                                      std::string_view program_version_id,
                                      std::string_view run_id,
                                      std::string_view workspace_revision) {
    return run.verify(program_version_id, run_id, workspace_revision);
}

void ContractBoundary::publish(Run& run) {
    run.publish();
}

}  // namespace neograph::harness
