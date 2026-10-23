#pragma once

// A fork's budget out of a paused run's remaining budget (#414).
//
// Amounts (wall time, tokens, money, operations, core steps, dynamic
// compiles) are split: the fork takes half and the source keeps the rest.
// max_concurrency is a limit, not an amount: the fork carries the source's
// own. The fork takes no child budget, and the retry ceiling is left unset.

#include <neograph/program/result.h>

#include <cstdint>

namespace neograph::test {

inline neograph::program::RunBudget fork_share(neograph::program::RunBudget remaining) {
    const auto half = [](auto value) { return value > 1 ? value / 2 : value; };
    remaining.wall_time_ms           = half(remaining.wall_time_ms);
    remaining.model_tokens           = half(remaining.model_tokens);
    remaining.monetary_microunits    = half(remaining.monetary_microunits);
    remaining.max_program_operations = half(remaining.max_program_operations);
    remaining.max_core_steps         = half(remaining.max_core_steps);
    remaining.max_dynamic_compiles   = half(remaining.max_dynamic_compiles);
    remaining.max_child_depth        = 0;
    remaining.max_total_children     = 0;
    remaining.max_node_retries.reset();
    return remaining;
}

// A fork's budget of `numerator`/`denominator` of each amount the source has left, for forks that take fixed shares of
// one pause: at least one of an amount the source has, none of one it has none of. max_concurrency is the source's own,
// the fork takes no child budget, and the retry ceiling is left unset.
inline neograph::program::RunBudget fork_fraction(neograph::program::RunBudget remaining, std::uint64_t numerator,
                                                  std::uint64_t denominator) {
    const auto part = [&](std::uint64_t value) -> std::uint64_t {
        return value == 0 ? 0 : (value * numerator / denominator > 1 ? value * numerator / denominator : 1);
    };
    remaining.wall_time_ms           = part(remaining.wall_time_ms);
    remaining.model_tokens           = part(remaining.model_tokens);
    remaining.monetary_microunits    = part(remaining.monetary_microunits);
    remaining.max_program_operations = part(remaining.max_program_operations);
    remaining.max_core_steps         = part(remaining.max_core_steps);
    remaining.max_dynamic_compiles   = part(remaining.max_dynamic_compiles);
    remaining.max_child_depth        = 0;
    remaining.max_total_children     = 0;
    remaining.max_node_retries.reset();
    return remaining;
}

// What the source has left after a fork took `share` of `remaining`: the
// amounts less the share, and max_concurrency as it was.
inline neograph::program::RunBudget fork_remainder(neograph::program::RunBudget remaining,
                                                   const neograph::program::RunBudget& share) {
    remaining.wall_time_ms -= share.wall_time_ms;
    remaining.model_tokens -= share.model_tokens;
    remaining.monetary_microunits -= share.monetary_microunits;
    remaining.max_program_operations -= share.max_program_operations;
    remaining.max_core_steps -= share.max_core_steps;
    remaining.max_dynamic_compiles -= share.max_dynamic_compiles;
    remaining.max_child_depth -= share.max_child_depth;
    remaining.max_total_children -= share.max_total_children;
    return remaining;
}

}  // namespace neograph::test
