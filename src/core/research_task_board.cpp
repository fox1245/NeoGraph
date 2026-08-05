#include <neograph/research/evidence_ledger.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::research {
namespace {

struct Candidate {
    ResearchTask task;
    double score = 0.0;
    double priority = 0.0;
    std::uint64_t cost = 1;
};

std::optional<double> number(const json& requirements, const char* key) {
    if (!requirements.contains(key)) return std::nullopt;
    const auto& value = requirements.at(key);
    if (!value.is_number()) return std::nullopt;
    const auto parsed = value.get<double>();
    if (!std::isfinite(parsed)) return std::nullopt;
    return parsed;
}

std::uint64_t task_cost(const ResearchTask& task) {
    const auto value = number(task.spec.requirements, "cost_microunits");
    if (!value || *value <= 0.0) return 1;
    if (*value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(std::ceil(*value));
}

bool terminal_stop(const ResearchTask& task, const ClaimResolution& resolution) {
    if (task.spec.kind == ResearchTaskKind::Reconciliation) return false;
    return resolution.kind == ClaimResolutionKind::Corroborated
        || resolution.kind == ClaimResolutionKind::Contradicted
        || resolution.kind == ClaimResolutionKind::HumanDecisionRequired;
}

} // namespace

std::optional<ResearchTaskLease> ResearchTaskBoard::acquire_next(
    std::string_view owner_scope, std::string_view worker_id,
    std::string_view board_id, std::uint64_t now_unix_ms,
    ResearchTaskBoardBudget budget) const {
    if (owner_scope.empty() || worker_id.empty() || board_id.empty()) {
        throw std::invalid_argument("research task board identities must not be empty");
    }
    if (now_unix_ms == 0) {
        throw std::invalid_argument("research task board time must be explicit and positive");
    }
    if (budget.max_active_leases == 0) return std::nullopt;

    ledger_.expire_leases(owner_scope, now_unix_ms);
    const auto snapshots = ledger_.tasks(owner_scope, true);

    std::uint32_t active = 0;
    std::uint64_t spent = 0;
    std::vector<Candidate> candidates;
    candidates.reserve(snapshots.size());
    for (const auto& task : snapshots) {
        const auto cost = task_cost(task);
        if (task.state == ResearchTaskState::Leased
            && task.lease_expires_at_unix_ms > now_unix_ms) {
            ++active;
            if (std::numeric_limits<std::uint64_t>::max() - spent < cost) {
                spent = std::numeric_limits<std::uint64_t>::max();
            } else {
                spent += cost;
            }
        }
        if (task.state == ResearchTaskState::Published) {
            if (std::numeric_limits<std::uint64_t>::max() - spent < cost) {
                spent = std::numeric_limits<std::uint64_t>::max();
            } else {
                spent += cost;
            }
        }
        if (task.state != ResearchTaskState::Ready) continue;
        if (!task.spec.claim_id.empty()
            && terminal_stop(task, ledger_.resolve_claim(owner_scope, task.spec.claim_id))) {
            continue;
        }
        const auto information = number(task.spec.requirements, "information_value").value_or(1.0);
        const auto priority = number(task.spec.requirements, "priority").value_or(0.0);
        if (information < 0.0 || priority < 0.0) continue;
        candidates.push_back(Candidate{task, information / static_cast<double>(cost),
                                       std::min(priority, 255.0), cost});
    }

    if (active >= budget.max_active_leases || spent >= budget.max_cost_microunits) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.score != rhs.score) return lhs.score > rhs.score;
        if (lhs.priority != rhs.priority) return lhs.priority > rhs.priority;
        return lhs.task.spec.task_id < rhs.task.spec.task_id;
    });

    for (const auto& candidate : candidates) {
        if (std::numeric_limits<std::uint64_t>::max() - spent < candidate.cost
            || spent + candidate.cost > budget.max_cost_microunits) {
            continue;
        }
        ResearchLeaseRequest request;
        request.task_id = candidate.task.spec.task_id;
        request.lease_id = std::string(board_id) + ":" + std::string(worker_id)
                         + ":" + candidate.task.spec.task_id;
        request.worker_id = worker_id;
        request.owner_scope = owner_scope;
        request.now_unix_ms = now_unix_ms;
        if (auto lease = ledger_.acquire_lease(std::move(request))) {
            return lease;
        }
    }
    return std::nullopt;
}

} // namespace neograph::research
