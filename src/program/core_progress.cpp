#include "core_progress.h"

namespace neograph::program::detail {

namespace {

bool is_synthetic_start(const graph::GraphEvent& event) {
    return (event.node_name == "__routing__" &&
            (event.data.contains("command_goto") || event.data.contains("next_nodes"))) ||
           (event.node_name == "__send__" && event.data.contains("sends"));
}

}  // namespace

void AttemptCoreProgress::observe(const graph::GraphEvent& event) noexcept {
    if (event.type != graph::GraphEvent::Type::NODE_START || is_synthetic_start(event)) return;

    if (!event.data.is_object() || !event.data.contains("step") ||
        !event.data["step"].is_number_integer()) {
        steps_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto value = event.data["step"].get<std::int64_t>();
    auto       prior = last_step_.load(std::memory_order_relaxed);
    while (prior != value) {
        if (last_step_.compare_exchange_weak(prior, value, std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            steps_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

}  // namespace neograph::program::detail
