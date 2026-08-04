#include <neograph/research/evidence_ledger.h>

#include <stdexcept>

namespace neograph::research {

std::string_view to_string(ResearchTaskKind kind) noexcept {
    switch (kind) {
        case ResearchTaskKind::PrimaryExtraction: return "primary-extraction";
        case ResearchTaskKind::IndependentReview: return "independent-review";
        case ResearchTaskKind::Reproduction: return "reproduction";
        case ResearchTaskKind::Rebuttal: return "rebuttal";
        case ResearchTaskKind::Reconciliation: return "reconciliation";
    }
    return "unknown";
}

ResearchTaskKind research_task_kind_from_string(std::string_view value) {
    if (value == "primary-extraction") return ResearchTaskKind::PrimaryExtraction;
    if (value == "independent-review") return ResearchTaskKind::IndependentReview;
    if (value == "reproduction") return ResearchTaskKind::Reproduction;
    if (value == "rebuttal") return ResearchTaskKind::Rebuttal;
    if (value == "reconciliation") return ResearchTaskKind::Reconciliation;
    throw std::invalid_argument("unknown research task kind");
}

std::string_view to_string(ResearchTaskState state) noexcept {
    switch (state) {
        case ResearchTaskState::Ready: return "ready";
        case ResearchTaskState::Leased: return "leased";
        case ResearchTaskState::Published: return "published";
        case ResearchTaskState::Cancelled: return "cancelled";
    }
    return "unknown";
}

ResearchTaskState research_task_state_from_string(std::string_view value) {
    if (value == "ready") return ResearchTaskState::Ready;
    if (value == "leased") return ResearchTaskState::Leased;
    if (value == "published") return ResearchTaskState::Published;
    if (value == "cancelled") return ResearchTaskState::Cancelled;
    throw std::invalid_argument("unknown research task state");
}

std::string_view to_string(EvidencePolarity polarity) noexcept {
    switch (polarity) {
        case EvidencePolarity::Supports: return "supports";
        case EvidencePolarity::Contradicts: return "contradicts";
        case EvidencePolarity::Inconclusive: return "inconclusive";
        case EvidencePolarity::NoSupport: return "no-support";
    }
    return "unknown";
}

EvidencePolarity evidence_polarity_from_string(std::string_view value) {
    if (value == "supports") return EvidencePolarity::Supports;
    if (value == "contradicts") return EvidencePolarity::Contradicts;
    if (value == "inconclusive") return EvidencePolarity::Inconclusive;
    if (value == "no-support") return EvidencePolarity::NoSupport;
    throw std::invalid_argument("unknown evidence polarity");
}

std::string_view to_string(SourceLifecycle state) noexcept {
    switch (state) {
        case SourceLifecycle::Unclaimed: return "unclaimed";
        case SourceLifecycle::Claimed: return "claimed";
        case SourceLifecycle::Extracted: return "extracted";
        case SourceLifecycle::Reviewed: return "reviewed";
        case SourceLifecycle::Corroborated: return "corroborated";
        case SourceLifecycle::Contradicted: return "contradicted";
        case SourceLifecycle::Inconclusive: return "inconclusive";
    }
    return "unknown";
}

std::string_view to_string(ClaimResolutionKind kind) noexcept {
    switch (kind) {
        case ClaimResolutionKind::Unresolved: return "unresolved";
        case ClaimResolutionKind::Corroborated: return "corroborated";
        case ClaimResolutionKind::Contradicted: return "contradicted";
        case ClaimResolutionKind::Inconclusive: return "inconclusive";
        case ClaimResolutionKind::ReconciliationRequired: return "reconciliation-required";
        case ClaimResolutionKind::HumanDecisionRequired: return "human-decision-required";
    }
    return "unknown";
}

} // namespace neograph::research
