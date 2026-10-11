#pragma once

#include <neograph/graph/types.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace neograph::program::detail {

/**
 * Counts executed Core supersteps from the canonical node-start stream and
 * records the nodes that completed. A failing Core rethrows without its
 * engine trace, so a failed attempt reports these names instead.
 */
class AttemptCoreProgress final {
public:
    void observe(const graph::GraphEvent& event) noexcept;

    std::uint64_t steps() const noexcept { return steps_.load(std::memory_order_relaxed); }

    /** Nodes that completed during this attempt, in completion order. */
    std::vector<std::string> completed_nodes() const;

private:
    static constexpr std::int64_t no_step = (std::numeric_limits<std::int64_t>::min)();

    std::atomic<std::int64_t>  last_step_{no_step};
    std::atomic<std::uint64_t> steps_{0};
    mutable std::mutex         completed_mutex_;
    std::vector<std::string>   completed_;
};

}  // namespace neograph::program::detail
