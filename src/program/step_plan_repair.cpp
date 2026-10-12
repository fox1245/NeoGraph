#include <neograph/program/step_plan_repair.h>

#include <neograph/graph/node.h>
#include <neograph/graph/state.h>

#include "canonical_json.h"

#include <asio/awaitable.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

constexpr std::size_t kReportViolationBytes = 8192;

// ----- The report both the node and the host helper keep -----

json violation_json(const StepPlanViolation& violation) {
    return json{{"path", violation.path},
                {"code", violation.code},
                {"message", violation.message},
                {"hint", violation.hint},
                {"detail", violation.detail}};
}

// The violations the last answer had, as many as fit in 8 KiB of JSON.
json capped_violations(const std::vector<StepPlanViolation>& violations) {
    json        result = json::array();
    std::size_t bytes  = 2;
    for (const auto& violation : violations) {
        auto item = violation_json(violation);
        const auto size = item.dump().size() + 1;
        if (bytes + size > kReportViolationBytes) break;
        bytes += size;
        result.push_back(std::move(item));
    }
    return result;
}

struct Ledger {
    std::string                         status = "retry";
    std::uint32_t                       attempts     = 0;
    std::uint32_t                       max_attempts = 3;
    json                                last_violations = json::array();
    std::map<std::string, std::uint64_t> histogram;
    std::vector<std::string>            digests;
    std::vector<std::string>            envelopes;

    static Ledger from(const json& report, std::uint32_t max_attempts) {
        Ledger ledger;
        ledger.max_attempts = max_attempts;
        if (!report.is_object()) return ledger;
        if (report.contains("attempts") && report["attempts"].is_number_unsigned())
            ledger.attempts = report["attempts"].get<std::uint32_t>();
        if (report.contains("violation_code_histogram") &&
            report["violation_code_histogram"].is_object()) {
            for (const auto& [code, count] : report["violation_code_histogram"].items())
                if (count.is_number_unsigned()) ledger.histogram[code] = count.get<std::uint64_t>();
        }
        const auto strings = [&](const char* key, std::vector<std::string>& out) {
            if (!report.contains(key) || !report[key].is_array()) return;
            for (std::size_t index = 0; index < report[key].size(); ++index)
                if (report[key][index].is_string()) out.push_back(report[key][index].get<std::string>());
        };
        strings("plan_digests", ledger.digests);
        strings("envelope", ledger.envelopes);
        return ledger;
    }

    void record(const StepPlanIntakeResult& result) {
        ++attempts;
        digests.push_back(result.plan_digest);
        envelopes.push_back(std::string(to_string(result.envelope)));
        if (result.ok()) {
            status          = "ok";
            last_violations = json::array();
            return;
        }
        for (const auto& violation : result.violations) ++histogram[violation.code];
        last_violations = capped_violations(result.violations);
        status          = attempts >= max_attempts ? "exhausted" : "retry";
    }

    json to_json() const {
        json counts = json::object();
        for (const auto& [code, count] : histogram) counts[code] = count;
        json plan_digests = json::array();
        for (const auto& digest : digests) plan_digests.push_back(digest);
        json envelope = json::array();
        for (const auto& value : envelopes) envelope.push_back(value);
        return json{{"status", status},
                    {"attempts", attempts},
                    {"max_attempts", max_attempts},
                    {"last_violations", last_violations},
                    {"violation_code_histogram", std::move(counts)},
                    {"plan_digests", std::move(plan_digests)},
                    {"envelope", std::move(envelope)}};
    }
};

// ----- Prompt rebuilding -----

std::string message_role(const json& message) {
    return message.is_object() && message.contains("role") && message["role"].is_string()
               ? message["role"].get<std::string>()
               : std::string();
}

std::string message_text(const json& message) {
    if (!message.is_object() || !message.contains("content")) return {};
    const auto content = message["content"];
    if (content.is_string()) return content.get<std::string>();
    std::string text;
    if (content.is_array()) {
        for (std::size_t index = 0; index < content.size(); ++index) {
            const auto part = content[index];
            if (part.is_object() && part.contains("text") && part["text"].is_string())
                text += part["text"].get<std::string>();
            else if (part.is_string())
                text += part.get<std::string>();
        }
    }
    return text;
}

std::string last_assistant_text(const json& messages) {
    if (!messages.is_array()) return {};
    for (std::size_t index = messages.size(); index-- > 0;) {
        if (message_role(messages[index]) == "assistant") return message_text(messages[index]);
    }
    return {};
}

json chat_message(std::string_view role, std::string_view content) {
    return json{{"role", std::string(role)}, {"content", std::string(content)}};
}

json rebuilt_messages(const json& messages, std::string_view feedback, StepPlanFeedback mode) {
    json result = json::array();
    if (!messages.is_array()) {
        result.push_back(chat_message("user", feedback));
        return result;
    }
    if (mode == StepPlanFeedback::Append) {
        for (std::size_t index = 0; index < messages.size(); ++index) result.push_back(messages[index]);
        result.push_back(chat_message("user", feedback));
        return result;
    }
    // Replace: [system messages, the first user message, the last answer, the violations].
    std::size_t index = 0;
    while (index < messages.size() && message_role(messages[index]) == "system")
        result.push_back(messages[index++]);
    for (; index < messages.size(); ++index) {
        if (message_role(messages[index]) == "user") {
            result.push_back(messages[index]);
            break;
        }
    }
    for (std::size_t back = messages.size(); back-- > 0;) {
        if (message_role(messages[back]) == "assistant") {
            result.push_back(messages[back]);
            break;
        }
    }
    result.push_back(chat_message("user", feedback));
    return result;
}

// ----- The registered executables -----

json options_json(const StepPlanRepairNodeOptions& options) {
    const auto budget = [](const TaskGraphBudget& value) {
        return json{{"wall_time_ms", value.wall_time_ms},
                    {"model_tokens", value.model_tokens},
                    {"monetary_microunits", value.monetary_microunits},
                    {"output_bytes", value.output_bytes}};
    };
    const auto strings = [](const std::vector<std::string>& values) {
        json result = json::array();
        for (const auto& value : values) result.push_back(value);
        return result;
    };
    json templates = json::array();
    for (const auto& contract : options.intake.template_allowlist) {
        json artifacts = json::array();
        for (const auto& artifact : contract.output_artifacts)
            artifacts.push_back(json{{"artifact", artifact.artifact}, {"fields", strings(artifact.fields)}});
        templates.push_back(json{{"template_id", contract.template_id},
                                 {"content_identity", contract.content_identity},
                                 {"input_fields", strings(contract.input_fields)},
                                 {"output_artifacts", std::move(artifacts)},
                                 {"budget_ceiling", budget(contract.budget_ceiling)},
                                 {"child_binding", contract.child_binding}});
    }
    json info = json::array();
    for (const auto& item : options.intake.template_info)
        info.push_back(json{{"template_id", item.template_id},
                            {"required_params", strings(item.required_params)},
                            {"summary", item.summary}});
    const auto& limits = options.intake.limits;
    const auto& proposal = options.intake.proposal_limits;
    return json{{"source_id", options.intake.source_id},
                {"limits", json{{"max_steps", limits.max_steps},
                                {"max_params_per_step", limits.max_params_per_step},
                                {"max_after_per_step", limits.max_after_per_step},
                                {"max_literal_bytes", limits.max_literal_bytes},
                                {"max_total_literal_bytes", limits.max_total_literal_bytes},
                                {"max_literal_depth", limits.max_literal_depth},
                                {"max_retry_attempts", limits.max_retry_attempts},
                                {"max_violations", limits.max_violations},
                                {"max_document_bytes", limits.max_document_bytes}}},
                {"proposal_limits", json{{"max_tasks", proposal.max_tasks},
                                         {"max_edges", proposal.max_edges},
                                         {"max_depth", proposal.max_depth},
                                         {"per_task", budget(proposal.per_task_budget_ceiling)},
                                         {"total", budget(proposal.total_budget_ceiling)},
                                         {"min_schema_version", proposal.min_schema_version},
                                         {"max_attempts", proposal.max_attempts},
                                         {"max_literal_bytes", proposal.max_literal_bytes},
                                         {"max_total_literal_bytes", proposal.max_total_literal_bytes}}},
                {"templates", std::move(templates)},
                {"template_info", std::move(info)},
                {"max_attempts_ceiling", options.max_attempts_ceiling},
                {"max_feedback_bytes", options.max_feedback_bytes}};
}

ExecutableManifest make_manifest(ExecutableKind kind, std::string_view name,
                                 const std::string& options_digest, const std::string& attestation) {
    return ExecutableManifest{
        {kind, std::string(name), "1.0.0",
         detail::sha256_identity("neograph step-plan repair executable/v1",
                                 std::string(name) + '\0' + options_digest)},
        EffectMode::Brokered,
        attestation,
        {},
        {},
        {}};
}

struct IntakeShared {
    StepPlanIntakeOptions intake;
    std::uint32_t         max_attempts_ceiling = 5;
    std::size_t           max_feedback_bytes   = 2048;
};

class IntakeNode final : public graph::GraphNode {
public:
    IntakeNode(std::string name, const json& config, std::shared_ptr<const IntakeShared> shared)
        : name_(std::move(name)), shared_(std::move(shared)) {
        for (const auto& [key, value] : config.items()) {
            if (key == "type") continue;
            if (key == "max_attempts") {
                if (!(value.is_number_unsigned() || value.is_number_integer()) ||
                    value.get<std::int64_t>() < 1 ||
                    value.get<std::int64_t>() > static_cast<std::int64_t>(shared_->max_attempts_ceiling))
                    throw std::invalid_argument(
                        "step_plan_intake max_attempts must be an integer from 1 to " +
                        std::to_string(shared_->max_attempts_ceiling));
                max_attempts_ = value.get<std::uint32_t>();
            } else if (key == "feedback") {
                const auto text = value.is_string() ? value.get<std::string>() : std::string();
                if (text == "replace") feedback_ = StepPlanFeedback::Replace;
                else if (text == "append") feedback_ = StepPlanFeedback::Append;
                else throw std::invalid_argument("step_plan_intake feedback must be \"replace\" or \"append\"");
            } else {
                throw std::invalid_argument("step_plan_intake has no configuration key '" + key + "'");
            }
        }
        max_attempts_ = std::min(max_attempts_, shared_->max_attempts_ceiling);
    }

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput input) override {
        graph::NodeOutput output;
        const auto messages = input.state.get("messages");
        auto       ledger   = Ledger::from(input.state.get("repair"), max_attempts_);
        const auto result   = intake_step_plan_text(last_assistant_text(messages), shared_->intake);
        ledger.record(result);

        graph::ChannelWrite proposal;
        proposal.channel = "proposal";
        proposal.mode    = graph::ChannelWrite::Mode::Overwrite;
        proposal.value   = result.ok() ? *result.proposal : json(nullptr);
        output.writes.push_back(std::move(proposal));

        if (ledger.status == "retry") {
            graph::ChannelWrite next;
            next.channel = "messages";
            next.mode    = graph::ChannelWrite::Mode::Overwrite;
            next.value   = rebuilt_messages(
                messages, format_violations_for_model(result.violations, shared_->max_feedback_bytes),
                feedback_);
            output.writes.push_back(std::move(next));
        }
        graph::ChannelWrite report;
        report.channel = "repair";
        report.mode    = graph::ChannelWrite::Mode::Overwrite;
        report.value   = ledger.to_json();
        output.writes.push_back(std::move(report));
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string                         name_;
    std::shared_ptr<const IntakeShared> shared_;
    std::uint32_t                       max_attempts_ = 3;
    StepPlanFeedback                    feedback_     = StepPlanFeedback::Replace;
};

class ReportNode final : public graph::GraphNode {
public:
    explicit ReportNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<graph::NodeOutput> run(graph::NodeInput input) override {
        graph::NodeOutput output;
        auto              report = input.state.get("repair");
        json              final_report = json::object();
        if (report.is_object()) {
            for (const auto& [key, value] : report.items()) final_report[key] = value;
        }
        final_report["status"] = "exhausted";
        graph::ChannelWrite write;
        write.channel = "repair";
        write.mode    = graph::ChannelWrite::Mode::Overwrite;
        write.value   = std::move(final_report);
        output.writes.push_back(std::move(write));
        co_return output;
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

std::string route_of(const graph::GraphState& state) {
    const auto report = state.get("repair");
    if (!report.is_object() || !report.contains("status") || !report["status"].is_string())
        return "give_up";
    const auto status = report["status"].get<std::string>();
    if (status == "ok") return "ok";
    if (status == "retry") return "retry";
    return "give_up";
}

}  // namespace

std::string_view to_string(StepPlanFeedback feedback) noexcept {
    switch (feedback) {
    case StepPlanFeedback::Replace: return "replace";
    case StepPlanFeedback::Append: return "append";
    }
    return "unknown";
}

void add_step_plan_repair(RegistrySnapshotBuilder& builder, const StepPlanRepairNodeOptions& options) {
    if (options.max_attempts_ceiling == 0)
        throw std::invalid_argument("max_attempts_ceiling must be positive");
    const auto options_digest =
        detail::sha256_identity("neograph step-plan repair options/v1",
                                detail::canonical_json_bytes(options_json(options)));
    auto shared = std::make_shared<IntakeShared>();
    shared->intake               = options.intake;
    shared->max_attempts_ceiling = options.max_attempts_ceiling;
    shared->max_feedback_bytes   = options.max_feedback_bytes;
    const std::shared_ptr<const IntakeShared> frozen = shared;

    const json config_schema{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         json{{"type", json{{"type", "string"}}},
              {"max_attempts", json{{"type", "integer"},
                                    {"minimum", 1},
                                    {"maximum", options.max_attempts_ceiling}}},
              {"feedback", json{{"enum", json::array({"replace", "append"})}}}}}};
    builder.add_node(
        make_manifest(ExecutableKind::Node, STEP_PLAN_INTAKE_NODE, options_digest, options.attestation_id),
        [frozen](const std::string& name, const json& config, const graph::NodeContext&) {
            return std::make_unique<IntakeNode>(name, config, frozen);
        },
        config_schema,
        json{{"writes", json::array({"messages", "proposal", "repair"})}});
    builder.add_node(
        make_manifest(ExecutableKind::Node, STEP_PLAN_REPORT_NODE, options_digest, options.attestation_id),
        [](const std::string& name, const json&, const graph::NodeContext&) {
            return std::make_unique<ReportNode>(name);
        },
        json{{"type", "object"}}, json{{"writes", json::array({"repair"})}});
    builder.add_condition(
        make_manifest(ExecutableKind::Condition, STEP_PLAN_ROUTE_CONDITION, options_digest,
                      options.attestation_id),
        &route_of, graph::ConditionSpec{{"ok", "retry", "give_up"}, false});
}

StepPlanRepairResult repair_step_plan(
    const PlanRepairOptions& options, std::string_view request,
    const std::function<PlannerReply(const std::vector<PlannerMessage>&)>& planner) {
    if (!planner) throw std::invalid_argument("repair_step_plan needs a planner callback");
    const auto max_attempts = std::max<std::uint32_t>(options.max_attempts, 1);
    StepPlanRepairResult result;
    Ledger               ledger;
    ledger.max_attempts = max_attempts;
    result.messages.push_back({"user", std::string(request)});

    while (true) {
        const auto reply = planner(result.messages);
        result.prompt_tokens += reply.prompt_tokens;
        result.completion_tokens += reply.completion_tokens;
        result.messages.push_back({"assistant", reply.text});

        const auto intake = intake_step_plan_text(reply.text, options.intake);
        ledger.record(intake);
        result.attempts = ledger.attempts;
        if (intake.ok()) {
            result.ok            = true;
            result.proposal      = *intake.proposal;
            result.proposal_hash = intake.proposal_hash;
            break;
        }
        if (ledger.status == "exhausted") break;

        const auto feedback = format_violations_for_model(intake.violations, options.max_feedback_bytes);
        if (options.feedback == StepPlanFeedback::Replace) {
            std::vector<PlannerMessage> next;
            std::size_t index = 0;
            while (index < result.messages.size() && result.messages[index].role == "system")
                next.push_back(result.messages[index++]);
            for (; index < result.messages.size(); ++index) {
                if (result.messages[index].role == "user") {
                    next.push_back(result.messages[index]);
                    break;
                }
            }
            for (std::size_t back = result.messages.size(); back-- > 0;) {
                if (result.messages[back].role == "assistant") {
                    next.push_back(result.messages[back]);
                    break;
                }
            }
            result.messages = std::move(next);
        }
        result.messages.push_back({"user", feedback});
    }
    result.repair = ledger.to_json();
    return result;
}

}  // namespace neograph::program
