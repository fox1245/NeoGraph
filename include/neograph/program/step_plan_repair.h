/**
 * @file program/step_plan_repair.h
 * @brief The plan repair loop: ask for a step plan, check it, feed the violations back, ask again.
 *
 * The loop is a documented pattern over mechanisms that already exist, not a new plan
 * operation. In a Program it is a Core graph `plan -> intake -> (plan again)`: a registered
 * pure node checks the planner's last answer with intake_step_plan and rebuilds the prompt, a
 * registered condition routes on the result, and the graph's own bounds (the attempt count,
 * `max_visits`, the run budget) end it. `add_step_plan_repair` registers the node, the
 * condition and a report node. A host that plans before it starts a run uses
 * `repair_step_plan`, the same loop around a callback.
 *
 * Channels the intake node reads and writes (names are fixed):
 *  - `messages`  read and rewritten: the conversation, as `{role, content}` messages. The last
 *                assistant message is the planner's answer.
 *  - `proposal`  written: the lowered TaskGraphProposal JSON when the plan is accepted, else null.
 *  - `repair`    read and written: the report below.
 *
 * The `repair` report is `{status, attempts, max_attempts, last_violations,
 * violation_code_histogram, plan_digests, envelope}`; `status` is `ok`, `retry` or `exhausted`.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/registry.h>
#include <neograph/program/step_plan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

inline constexpr std::string_view STEP_PLAN_INTAKE_NODE      = "neograph.step_plan_intake/v1";
inline constexpr std::string_view STEP_PLAN_ROUTE_CONDITION  = "neograph.step_plan_route/v1";
inline constexpr std::string_view STEP_PLAN_REPORT_NODE      = "neograph.step_plan_report/v1";

/// How the next prompt is built after a rejected plan.
enum class StepPlanFeedback : std::uint8_t {
    /// Rebuild the conversation as [system messages, first user message, the rejected plan,
    /// the violations]. The context does not grow and an earlier mistake is not re-read.
    Replace,
    /// Keep the whole conversation and add the violations to it.
    Append,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(StepPlanFeedback feedback) noexcept;

struct StepPlanRepairNodeOptions {
    /// Trusted, host-authored: the allow-list, limits and template facts. The sealed graph
    /// cannot change them; its node configuration only picks `max_attempts` and `feedback`.
    StepPlanIntakeOptions intake;
    /// Host declaration id recorded in the executables' manifests.
    std::string attestation_id = "host:step-plan-repair";
    /// Host ceiling for a node's `max_attempts`.
    std::uint32_t max_attempts_ceiling = 5;
    /// Size cap of the violation text added to the prompt.
    std::size_t max_feedback_bytes = 2048;
};

/**
 * Register the intake node (`neograph.step_plan_intake/v1`; configuration `max_attempts`, 1 to
 * the host ceiling, default 3, and `feedback`, "replace" (default) or "append"), the route
 * condition (`neograph.step_plan_route/v1`: `ok`, `retry`, `give_up`) and the report node
 * (`neograph.step_plan_report/v1`: a final, idempotent write of the exhausted report, for a
 * graph that ends the loop in a failure exit). The implementation identities cover the host
 * options, so a Program admitted with one allow-list does not run with another.
 */
NEOGRAPH_PROGRAM_API void add_step_plan_repair(RegistrySnapshotBuilder&          builder,
                                               const StepPlanRepairNodeOptions& options);

/** One message of the planner's conversation. */
struct PlannerMessage {
    std::string role;     ///< "system", "user" or "assistant"
    std::string content;

    bool operator==(const PlannerMessage&) const = default;
};

/** What a planner call returns; usage is the host's own accounting. */
struct PlannerReply {
    std::string   text;
    std::uint64_t prompt_tokens     = 0;
    std::uint64_t completion_tokens = 0;
};

struct PlanRepairOptions {
    StepPlanIntakeOptions intake;
    std::uint32_t         max_attempts       = 3;
    StepPlanFeedback      feedback           = StepPlanFeedback::Replace;
    std::size_t           max_feedback_bytes = 2048;
};

struct StepPlanRepairResult {
    bool                        ok = false;
    /// The lowered proposal of the accepted plan; null otherwise.
    json                        proposal = nullptr;
    std::optional<std::string>  proposal_hash;
    /// The report, with the same shape the intake node writes to the `repair` channel.
    json                        repair = json::object();
    std::uint32_t               attempts = 0;
    std::uint64_t               prompt_tokens     = 0;
    std::uint64_t               completion_tokens = 0;
    /// The conversation as the planner last saw it.
    std::vector<PlannerMessage> messages;
};

/**
 * Run the loop around a planner callback, for hosts that plan before they start a run. The
 * accepted proposal then becomes the run's input, which the run record persists, so a resume
 * never plans again. Accounting is the host's own: the run's budgets do not include planning.
 * `request` is the first user message and should carry the catalog the planner may use.
 */
NEOGRAPH_PROGRAM_API StepPlanRepairResult
repair_step_plan(const PlanRepairOptions&                                           options,
                 std::string_view                                                   request,
                 const std::function<PlannerReply(const std::vector<PlannerMessage>&)>& planner);

}  // namespace neograph::program
