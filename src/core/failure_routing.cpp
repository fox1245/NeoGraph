#include "failure_routing.h"

#include <algorithm>
#include <utility>

namespace neograph::graph::detail {

FailureRouting FailureRouting::build(const std::map<std::string, NodeRetryPolicySpec>& policies,
                                     const std::vector<FailureEdge>&                   edges,
                                     const std::map<std::string, FailureExit>&         exits,
                                     FailureTextRedactor                               redactor) {
    FailureRouting routing;
    for (const auto& [node, spec] : policies) {
        auto& plan = routing.nodes[node];
        plan.non_retryable.insert(spec.non_retryable.begin(), spec.non_retryable.end());
        plan.max_visits = spec.max_visits;
        plan.error_text = spec.error_text.value_or(false);
    }
    for (const auto& edge : edges) {
        auto& plan         = routing.nodes[edge.from];
        plan.target        = edge.to;
        plan.error_channel = edge.error_channel;
        routing.targets[edge.from] = edge.to;
    }
    for (const auto& [node, plan] : routing.nodes) {
        if (plan.max_visits) routing.visit_limits = true;
        if (plan.max_visits && !plan.target.empty()) routing.visit_resets[plan.target].push_back(node);
    }
    for (const auto& [node, exit] : exits)
        routing.exits[node] = exit.code.empty() ? std::string(kAbortedCode) : exit.code;
    routing.redactor = std::move(redactor);
    return routing;
}

bool FailureRunState::activate(const FailureRouting& routing, const std::string& node) {
    const auto* plan = routing.plan(node);
    if (!plan || !plan->max_visits) return true;
    std::lock_guard lock(mutex_);
    auto& visits = visits_[node];
    if (visits >= *plan->max_visits) {
        if (visits == 0) visits_.erase(node);
        return false;
    }
    ++visits;
    return true;
}

void FailureRunState::target_completed(const FailureRouting& routing, const std::string& target) {
    const auto found = routing.visit_resets.find(target);
    if (found == routing.visit_resets.end()) return;
    std::lock_guard lock(mutex_);
    for (const auto& node : found->second) visits_.erase(node);
}

json FailureRunState::encode(const std::map<std::string, int>& visits) {
    json encoded;
    for (const auto& [node, count] : visits)
        if (count > 0) encoded[node] = count;
    return encoded;
}

json FailureRunState::visits() const {
    std::lock_guard lock(mutex_);
    return encode(visits_);
}

void FailureRunState::mark_step_start() {
    std::lock_guard lock(mutex_);
    step_start_ = visits_;
}

json FailureRunState::step_start_visits() const {
    std::lock_guard lock(mutex_);
    return encode(step_start_);
}

void FailureRunState::restore(const json& metadata) {
    std::lock_guard lock(mutex_);
    visits_.clear();
    if (!metadata.is_object() || !metadata.contains("_neograph")) return;
    const auto& owned = metadata["_neograph"];
    if (!owned.is_object() || !owned.contains("node_visits")) return;
    const auto& stored = owned["node_visits"];
    if (!stored.is_object()) return;
    for (const auto& [node, count] : stored.items())
        if (count.is_number_integer() && count.get<std::int64_t>() > 0)
            visits_[node] = static_cast<int>(std::min<std::int64_t>(count.get<std::int64_t>(), 1 << 20));
    step_start_ = visits_;
}

const json* failure_route_record(const NodeResult& result) noexcept {
    for (const auto& write : result.writes)
        if (write.channel == kFailureRouteChannel) return &write.value;
    return nullptr;
}

const std::vector<ChannelWrite>& state_writes(const std::vector<ChannelWrite>& writes,
                                              std::vector<ChannelWrite>&       storage) {
    const auto marker = [](const ChannelWrite& write) { return write.channel == kFailureRouteChannel; };
    if (std::none_of(writes.begin(), writes.end(), marker)) return writes;
    storage.clear();
    for (const auto& write : writes)
        if (!marker(write)) storage.push_back(write);
    return storage;
}

json failure_record_json(const NodeFailure& failure, int step, const std::string& routed_to,
                         const std::string& text) {
    json effects = json::array();
    for (const auto& effect : failure.effects)
        effects.push_back(json{{"tool", effect.tool}, {"ordinal", effect.ordinal}, {"status", effect.status}});
    json record{{"node", failure.node},
                {"kind", to_string(failure.kind)},
                {"code", failure.code},
                {"attempts", failure.attempts},
                {"retryable", failure.retryable},
                {"step", step},
                {"routed_to", routed_to},
                {"effects", std::move(effects)}};
    if (!text.empty()) record["detail"] = json{{"untrusted_text", text}};
    return record;
}

}  // namespace neograph::graph::detail
