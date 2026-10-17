/**
 * @file node_retry_budget.h
 * @brief Internal: RunBudget::max_node_retries and ProgramUsage::node_retries (#364).
 *
 * The run-level retry ceiling is optional: absent means none. Every budget
 * codec writes it only when it is set and accepts it when present, so budgets
 * without it keep their exact bytes and identities. In budget comparisons an
 * absent ceiling is the largest value.
 *
 * The ceiling belongs to one run. It only ever stops retries, so it is never
 * reserved, debited or summed across children, and a run's remaining ceiling
 * never grows across its transitions. A child run has its own: a child that
 * ng.spawn starts inherits its parent's granted ceiling, and a child started
 * or synthesized with an explicit invocation budget uses that budget's.
 */
#pragma once

#include <neograph/json.h>
#include <neograph/program/result.h>

#include "../core/json_builder.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace neograph::program::detail {

inline constexpr const char* kMaxNodeRetries = "max_node_retries";
inline constexpr const char* kNodeRetries    = "node_retries";

/// `value` is within `limit`: an absent limit admits anything, an absent
/// value (none) exceeds every set limit.
inline bool node_retries_within(const std::optional<std::uint64_t>& value,
                                const std::optional<std::uint64_t>& limit) noexcept {
    return !limit || (value && *value <= *limit);
}

/// The tighter of two ceilings.
inline std::optional<std::uint64_t> tighter_node_retries(const std::optional<std::uint64_t>& left,
                                                         const std::optional<std::uint64_t>& right) {
    if (!left) return right;
    if (!right) return left;
    return std::min(*left, *right);
}

/// Writes "max_node_retries" only when the budget sets it.
inline void encode_node_retries(json& encoded, const RunBudget& budget) {
    if (budget.max_node_retries) encoded[kMaxNodeRetries] = *budget.max_node_retries;
}

/// The same member, in a body built with neograph::detail::JsonBuilder (#478).
inline void encode_node_retries(::neograph::detail::JsonBuilder& builder,
                                ::neograph::detail::JsonBuilder::Value encoded,
                                const RunBudget& budget) {
    if (budget.max_node_retries)
        builder.set(encoded, kMaxNodeRetries, builder.make(*budget.max_node_retries));
}

/// Reads an optional "max_node_retries" (an unsigned integer).
inline std::optional<std::uint64_t> decode_node_retries(const json& encoded) {
    if (!encoded.is_object() || !encoded.contains(kMaxNodeRetries)) return std::nullopt;
    const auto value = encoded[kMaxNodeRetries];
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument("Program budget max_node_retries must be an unsigned integer");
}

/// Writes "node_retries" only when nonzero.
inline void encode_node_retry_usage(json& encoded, const ProgramUsage& usage) {
    if (usage.node_retries != 0) encoded[kNodeRetries] = usage.node_retries;
}

/// Reads an optional "node_retries" (zero when absent).
inline std::uint64_t decode_node_retry_usage(const json& encoded) {
    if (!encoded.is_object() || !encoded.contains(kNodeRetries)) return 0;
    const auto value = encoded[kNodeRetries];
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<std::uint64_t>(value.get<std::int64_t>());
    throw std::invalid_argument("Program usage node_retries must be an unsigned integer");
}

/// The ceiling left after `used` retries.
inline std::optional<std::uint64_t> remaining_node_retries(const std::optional<std::uint64_t>& granted,
                                                           std::uint64_t used) noexcept {
    if (!granted) return std::nullopt;
    return used >= *granted ? 0 : *granted - used;
}

}  // namespace neograph::program::detail
