/**
 * @file budget_overrun.h
 * @brief Internal: the failure record of a run that went over a budget (#420, #443).
 *
 * Four budgets are checked against a run's usage after the fact: wall time,
 * model tokens, money and Core steps. Each ends the run in the same shape: a
 * failure record whose witness holds `{granted, used, over_by}` under the
 * budget's own key, so a caller reads which budget a run exceeded, and by how
 * much, from `failure.witness` and not from the status.
 *
 * | budget       | status          | failure.code        | witness key |
 * |--------------|-----------------|---------------------|-------------|
 * | wall time    | TimedOut        | P_RUNTIME_TIMEOUT   | wall_time_ms |
 * | model tokens | BudgetExhausted | NG.BudgetExhausted  | model_tokens |
 * | money        | BudgetExhausted | P_MONEY_BUDGET      | monetary     |
 * | Core steps   | BudgetExhausted | P_CORE_STEP_BUDGET  | core_steps   |
 *
 * The codes are the ones each budget already ended a run with when it was
 * stopped on the way (the deadline, a refused priced call, the Core engine's
 * step limit, a refused or over-budget model call). `over_by` is how far the
 * usage went past the grant, and 0 when the run was stopped at the grant.
 *
 * A run that completes its plan can be over several budgets at once. The
 * record is deterministic: the code and status are those of the first budget
 * in the order of the table (the order of the RunBudget fields: wall time,
 * model tokens, money, Core steps), and the witness holds an entry for every
 * budget that was exceeded. Wall time comes first because the deadline path
 * ends a run TimedOut whatever else the run exceeded.
 *
 * Exported for the runtime and its tests only; no compatibility promise.
 */
#pragma once

#include <neograph/graph/types.h>
#include <neograph/json.h>
#include <neograph/program/result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace neograph::program::detail {

/// `{granted, used, over_by}`: how a run stands against one budget.
inline json budget_witness(std::uint64_t granted, std::uint64_t used) {
    return json{{"granted", granted},
                {"used", used},
                {"over_by", used > granted ? used - granted : std::uint64_t{0}}};
}

inline ProgramFailure budget_failure(std::string code, std::string message, json witness) {
    return ProgramFailure{std::move(code), std::move(message), "root", "", 0, std::move(witness)};
}

/// The failure of a run that its wall-time budget stopped. `witness` is what
/// the caller already knows; the budget's own entry is added to it.
inline ProgramFailure wall_time_failure(std::uint64_t granted,
                                        std::uint64_t used,
                                        json          witness = json::object()) {
    if (!witness.is_object()) witness = json::object();
    witness["wall_time_ms"] = budget_witness(granted, used);
    return budget_failure("P_RUNTIME_TIMEOUT",
                          "Program wall-time budget (" + std::to_string(granted) + " ms) expired",
                          std::move(witness));
}

/// The failure of a run that the model-token budget stopped and that no node
/// reported, because the last reply took it over the budget, or because the
/// next call did not fit. Its code is the one the failure classifier gives
/// budget exhaustion, which is what a call refused before it was sent already
/// ends with, so a caller reads one shape (and the protocol hosts report one
/// class) for both.
inline ProgramFailure model_token_budget_failure(std::uint64_t granted,
                                                 std::uint64_t used,
                                                 json          witness = json::object()) {
    if (!witness.is_object()) witness = json::object();
    witness["model_tokens"] = budget_witness(granted, used);
    auto message = used > granted
        ? "Model-token budget exceeded by the last reply: " + std::to_string(used) +
              " tokens used of " + std::to_string(granted)
        : "Model-token budget exhausted: " + std::to_string(used) + " tokens used of " +
              std::to_string(granted) + ", and the next call does not fit";
    return budget_failure(graph::node_failure_code(graph::NodeFailureClass::BudgetExhausted),
                          std::move(message), std::move(witness));
}

/// The failure of a run whose priced calls were charged more than its money
/// budget, because a reported cost was above the call's reservation. A call
/// refused before it was sent keeps its own P_MONEY_BUDGET record, whose
/// witness holds the `monetary` evidence of the refusal.
inline ProgramFailure money_budget_failure(std::uint64_t granted,
                                           std::uint64_t used,
                                           json          witness = json::object()) {
    if (!witness.is_object()) witness = json::object();
    witness["monetary"] = budget_witness(granted, used);
    return budget_failure("P_MONEY_BUDGET",
                          "Monetary budget exceeded: " + std::to_string(used) +
                              " microunits charged of " + std::to_string(granted),
                          std::move(witness));
}

/// The failure of a run that took more Core steps than its Core-step budget.
/// The Core engine stops a graph at the steps that were left when it started
/// and counts only its own super-steps, while the run counts the steps of its
/// subgraph nodes too, so a graph can reach its end over the run's grant.
inline ProgramFailure core_step_budget_failure(std::uint64_t granted,
                                               std::uint64_t used,
                                               json          witness = json::object()) {
    if (!witness.is_object()) witness = json::object();
    witness["core_steps"] = budget_witness(granted, used);
    return budget_failure("P_CORE_STEP_BUDGET",
                          "Core step budget exceeded: " + std::to_string(used) +
                              " steps run of " + std::to_string(granted),
                          std::move(witness));
}

/// An entry for each budget that the usage is over, as `{granted, used, over_by}`
/// under the budget's key: an object, empty when the run is within every budget.
///
/// `check_model_tokens` is false for a recorded replay, whose provider calls
/// are captured and spend nothing. `money_exceeded` is the monetary account's
/// own verdict (the amounts charged and held are above its ceiling).
inline json exceeded_budgets(const RunBudget&    granted,
                             const ProgramUsage& usage,
                             bool                check_model_tokens,
                             bool                money_exceeded) {
    json witness = json::object();
    if (usage.wall_time_ms > granted.wall_time_ms)
        witness["wall_time_ms"] = budget_witness(granted.wall_time_ms, usage.wall_time_ms);
    if (check_model_tokens && usage.model_tokens > granted.model_tokens)
        witness["model_tokens"] = budget_witness(granted.model_tokens, usage.model_tokens);
    if (money_exceeded)
        witness["monetary"] =
            budget_witness(granted.monetary_microunits, usage.monetary_microunits);
    if (usage.core_steps > granted.max_core_steps)
        witness["core_steps"] = budget_witness(granted.max_core_steps, usage.core_steps);
    return witness;
}

/// How a run that completed its plan ends when it is over a budget.
struct BudgetOverrun {
    ProgramTerminalStatus status = ProgramTerminalStatus::BudgetExhausted;
    ProgramFailure        failure;
};

/**
 * The ending of a run whose plan completed, from its usage against its grant;
 * none when it is within every budget. Arguments as for exceeded_budgets().
 */
inline std::optional<BudgetOverrun> completed_run_overrun(const RunBudget&    granted,
                                                          const ProgramUsage& usage,
                                                          bool                check_model_tokens,
                                                          bool                money_exceeded) {
    auto witness = exceeded_budgets(granted, usage, check_model_tokens, money_exceeded);
    if (witness.empty()) return std::nullopt;

    // Every exceeded budget is in the witness; the first of them, in the order
    // of the table above, gives the status and the code.
    if (witness.contains("wall_time_ms"))
        return BudgetOverrun{ProgramTerminalStatus::TimedOut,
                             wall_time_failure(granted.wall_time_ms, usage.wall_time_ms,
                                               std::move(witness))};
    if (witness.contains("model_tokens"))
        return BudgetOverrun{ProgramTerminalStatus::BudgetExhausted,
                             model_token_budget_failure(granted.model_tokens, usage.model_tokens,
                                                        std::move(witness))};
    if (witness.contains("monetary"))
        return BudgetOverrun{ProgramTerminalStatus::BudgetExhausted,
                             money_budget_failure(granted.monetary_microunits,
                                                  usage.monetary_microunits, std::move(witness))};
    return BudgetOverrun{ProgramTerminalStatus::BudgetExhausted,
                         core_step_budget_failure(granted.max_core_steps, usage.core_steps,
                                                  std::move(witness))};
}

/// Whether `code` is the code of one of the four budget records above.
inline bool is_budget_failure_code(const std::string& code) {
    return code == "P_RUNTIME_TIMEOUT" || code == "P_MONEY_BUDGET" ||
           code == "P_CORE_STEP_BUDGET" ||
           code == graph::node_failure_code(graph::NodeFailureClass::BudgetExhausted);
}

/**
 * Completes the record of a run that a budget stopped on the way (the
 * deadline, a refused call, the Core engine's step limit): the Core-step and
 * timeout records say how the run stands against its grant, as the others do,
 * and every other budget that the run is over by the end is in the witness
 * too. An entry the record already has is kept.
 */
inline void complete_budget_record(ProgramFailure&      failure,
                                   const RunBudget&     granted,
                                   const ProgramUsage&  usage,
                                   bool                 check_model_tokens,
                                   bool                 money_exceeded) {
    if (!is_budget_failure_code(failure.code)) return;
    if (!failure.witness.is_object()) failure.witness = json::object();
    if (failure.code == "P_CORE_STEP_BUDGET" && !failure.witness.contains("core_steps"))
        failure.witness["core_steps"] = budget_witness(granted.max_core_steps, usage.core_steps);
    if (failure.code == "P_RUNTIME_TIMEOUT" && !failure.witness.contains("wall_time_ms"))
        failure.witness["wall_time_ms"] = budget_witness(granted.wall_time_ms, usage.wall_time_ms);
    const auto exceeded = exceeded_budgets(granted, usage, check_model_tokens, money_exceeded);
    for (const auto& [key, entry] : exceeded.items())
        if (!failure.witness.contains(key)) failure.witness[key] = entry;
}

}  // namespace neograph::program::detail
