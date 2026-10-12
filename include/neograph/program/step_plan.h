/**
 * @file program/step_plan.h
 * @brief Flat step-plan intake: `step-plan/v1` checked and lowered into a TaskGraphProposal.
 *
 * A language model writes a short ordered list of steps. `intake_step_plan` is a pure function:
 * it checks the document against a closed schema and the host's template allow-list, reports
 * every problem as a machine-readable violation, and lowers a valid plan, byte for byte
 * deterministically, into the JSON of a schema-2 `TaskGraphProposal`. The proposal then takes
 * the existing trusted path (`TaskGraphProposal::parse`, `TaskGraphFragmentCompiler::compile`,
 * publication); the step plan itself is never executed and carries no authority.
 *
 * Passing intake means the plan is well formed and inside policy. It does not mean the plan
 * solves the request.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/source.h>
#include <neograph/program/task_graph_proposal.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace neograph::program {

/** One problem found in a submitted plan. The list is meant to be fed back to the planner. */
struct StepPlanViolation {
    /// JSON Pointer into the submitted document; "" is the document.
    std::string path;
    /// Stable code, always `STEP_*`.
    std::string code;
    /// One sentence. It names ids, templates and codes only; plan-derived strings are cut to
    /// 64 printable bytes and literal values are never echoed.
    std::string message;
    /// One imperative sentence; lists at most five valid names.
    std::string hint;
    /// Machine fields: given, expected, choices, and so on.
    json detail = json::object();

    bool operator==(const StepPlanViolation&) const = default;
};

/** Ceilings on the size and shape of a submitted plan. */
struct StepPlanLimits {
    std::uint32_t max_steps               = 32;
    std::uint32_t max_params_per_step     = 16;
    std::uint32_t max_after_per_step      = 8;
    std::uint32_t max_literal_bytes       = 4096;
    std::uint32_t max_total_literal_bytes = 32768;
    std::uint32_t max_literal_depth       = 6;
    std::uint32_t max_retry_attempts      = 3;
    std::uint32_t max_violations          = 32;
    std::uint32_t max_document_bytes      = 131072;
};

/**
 * Advisory, host-authored facts about one template. They may only add rejections: the trusted
 * allow-list stays the authority.
 */
struct StepPlanTemplateInfo {
    std::string              template_id;
    /// Input names without the leading '/'.
    std::vector<std::string> required_params;
    /// At most 160 bytes; shown to the planner by catalog views.
    std::string              summary;

    bool operator==(const StepPlanTemplateInfo&) const = default;
};

struct StepPlanIntakeOptions {
    /// Source id of the plan; diagnostics and the source map name it.
    std::string                            source_id = "step-plan";
    StepPlanLimits                         limits;
    /// The same object the host gives TaskGraphProposal::parse. Its literal and attempt limits
    /// bound what a lowered plan may carry.
    TaskGraphProposalLimits                proposal_limits;
    /// The same trusted list the host gives parse and compile.
    std::vector<TaskGraphTemplateContract> template_allowlist;
    std::vector<StepPlanTemplateInfo>      template_info;
};

/** How the JSON object was found in the planner's text. */
enum class StepPlanEnvelope : std::uint8_t {
    /// The input was a document, or no object was found.
    None,
    /// The whole text is the object.
    Strict,
    /// The object came from a fenced block, or from text around it.
    Recovered,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(StepPlanEnvelope envelope) noexcept;

struct StepPlanIntakeResult {
    /// Sorted (numeric-aware by path, then code, then detail), capped, stable.
    std::vector<StepPlanViolation> violations;
    StepPlanEnvelope               envelope = StepPlanEnvelope::None;
    /// sha256 identity of the canonical step plan; empty when the input was not JSON.
    std::string                    plan_digest;
    /// The lowered TaskGraphProposal JSON, in canonical form; set only when ok().
    std::optional<json>            proposal;
    /// TaskGraphProposal::proposal_hash() of that JSON.
    std::optional<std::string>     proposal_hash;
    /// Canonical lowered pointer -> authored step pointer.
    std::vector<SourceMapEntry>    source_map;
    /// {steps, edges, depth, width, literal_bytes} of an accepted plan.
    json                           report = json::object();

    bool ok() const noexcept { return violations.empty() && proposal.has_value(); }
};

/** Intake of the planner's text: the first top-level JSON object, in a fence or after prose. */
NEOGRAPH_PROGRAM_API StepPlanIntakeResult
intake_step_plan_text(std::string_view text, const StepPlanIntakeOptions& options);

/** Intake of an already parsed document. Duplicate keys cannot be seen here. */
NEOGRAPH_PROGRAM_API StepPlanIntakeResult
intake_step_plan_document(const json& document, const StepPlanIntakeOptions& options);

inline StepPlanIntakeResult intake_step_plan(std::string_view            text,
                                             const StepPlanIntakeOptions& options) {
    return intake_step_plan_text(text, options);
}

template <class Document,
          std::enable_if_t<std::is_same_v<std::remove_cv_t<std::remove_reference_t<Document>>, json>,
                           int> = 0>
StepPlanIntakeResult intake_step_plan(const Document& document,
                                      const StepPlanIntakeOptions& options) {
    return intake_step_plan_document(document, options);
}

/**
 * Deterministic text for the planner's next prompt, at most `max_bytes` long: one numbered line
 * per violation, then a note when some are left out.
 */
NEOGRAPH_PROGRAM_API std::string
format_violations_for_model(const std::vector<StepPlanViolation>& violations,
                            std::size_t                           max_bytes);

/**
 * A JSON Schema (2020-12) specialised to a catalog, for providers that enforce structured
 * output: `use` is an enum of the allowed template ids and `params` lists each template's input
 * names. It is a generated aid; the validator in intake_step_plan stays the authority.
 */
NEOGRAPH_PROGRAM_API json
step_plan_json_schema(const std::vector<TaskGraphTemplateContract>& templates,
                      const StepPlanLimits&                         limits);

}  // namespace neograph::program
