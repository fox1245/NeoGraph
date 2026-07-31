#pragma once

#include <neograph/graph/types.h>

#include <atomic>
#include <cstdint>
#include <limits>

namespace neograph::program::detail {

/** Counts executed Core supersteps from the canonical node-start stream. */
class AttemptCoreProgress final {
public:
    void observe(const graph::GraphEvent& event) noexcept;

    std::uint64_t steps() const noexcept { return steps_.load(std::memory_order_relaxed); }

private:
    static constexpr std::int64_t no_step = (std::numeric_limits<std::int64_t>::min)();

    std::atomic<std::int64_t>  last_step_{no_step};
    std::atomic<std::uint64_t> steps_{0};
};

}  // namespace neograph::program::detail
