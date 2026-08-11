#include <neograph/program/invocation.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {
constexpr std::string_view INVOCATION_FORMAT = "neograph-program-run-invocation";
constexpr std::string_view TERMINAL_FORMAT = "neograph-program-invocation-terminal";

std::string required_string(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value[key].is_string()) {
        throw std::invalid_argument("Invocation field '" + key + "' must be a string");
    }
    return value[key].get<std::string>();
}

std::uint64_t required_unsigned(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key) || !value[key].is_number_unsigned()) {
        throw std::invalid_argument("Invocation field '" + key + "' must be unsigned");
    }
    return value[key].get<std::uint64_t>();
}

std::uint32_t required_u32(const json& value, std::string_view field) {
    const auto number = required_unsigned(value, field);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Invocation field '" + std::string(field) +
                                    "' exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

json required_value(const json& value, std::string_view field) {
    const std::string key(field);
    if (!value.contains(key)) {
        throw std::invalid_argument("Invocation field '" + key + "' is required");
    }
    return value[key];
}

json encode_budget(const RunBudget& budget) {
    return json{{"wall_time_ms", budget.wall_time_ms},
                {"model_tokens", budget.model_tokens},
                {"monetary_microunits", budget.monetary_microunits},
                {"max_concurrency", budget.max_concurrency},
                {"max_program_operations", budget.max_program_operations},
                {"max_core_steps", budget.max_core_steps},
                {"max_dynamic_compiles", budget.max_dynamic_compiles},
                {"max_child_depth", budget.max_child_depth},
                {"max_total_children", budget.max_total_children}};
}

RunBudget decode_budget(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Invocation budget must be an object");
    detail::reject_unknown_fields(value,
                                  "Invocation budget",
                                  {"wall_time_ms",
                                   "model_tokens",
                                   "monetary_microunits",
                                   "max_concurrency",
                                   "max_program_operations",
                                   "max_core_steps",
                                   "max_dynamic_compiles",
                                   "max_child_depth",
                                   "max_total_children"});
    return RunBudget{required_unsigned(value, "wall_time_ms"),
                     required_unsigned(value, "model_tokens"),
                     required_unsigned(value, "monetary_microunits"),
                     required_u32(value, "max_concurrency"),
                     required_unsigned(value, "max_program_operations"),
                     required_unsigned(value, "max_core_steps"),
                     required_unsigned(value, "max_dynamic_compiles"),
                     required_u32(value, "max_child_depth"),
                     required_unsigned(value, "max_total_children")};
}

json encode_artifact(const InvocationArtifactReference& artifact) {
    return json{{"artifact_identity", artifact.artifact_identity},
                {"uri", artifact.uri},
                {"media_type", artifact.media_type},
                {"size_bytes", artifact.size_bytes}};
}

InvocationArtifactReference decode_artifact(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Invocation artifact must be an object");
    detail::reject_unknown_fields(
        value, "Invocation artifact", {"artifact_identity", "uri", "media_type", "size_bytes"});
    return InvocationArtifactReference{required_string(value, "artifact_identity"),
                                       required_string(value, "uri"),
                                       required_string(value, "media_type"),
                                       required_unsigned(value, "size_bytes")};
}

json invocation_payload(const RunInvocation& invocation) {
    return json{{"protocol_revision", invocation.protocol_revision},
                {"owner_scope", invocation.owner_scope},
                {"agent_id", invocation.agent_id},
                {"program_version_id", invocation.program_version_id},
                {"run_id", invocation.run_id},
                {"parent_run_id", invocation.parent_run_id},
                {"budget", encode_budget(invocation.budget)},
                {"input", invocation.input},
                {"message_sequence", invocation.message_sequence},
                {"correlation_id", invocation.correlation_id},
                {"artifact", invocation.artifact ? encode_artifact(*invocation.artifact) : json(nullptr)}};
}

void validate_diagnostic(const InvocationDiagnostic& diagnostic) {
    detail::validate_token(diagnostic.code, "Invocation diagnostic code");
    detail::validate_utf8(diagnostic.message);
    const auto canonical = std::string(to_string(diagnostic.severity));
    if (diagnostic.severity == InvocationDiagnosticSeverity::Unknown) {
        if (diagnostic.severity_value.empty()) {
            throw std::invalid_argument("Unknown invocation diagnostic severity must preserve its value");
        }
        detail::validate_token(diagnostic.severity_value, "Invocation diagnostic severity");
    } else if (!diagnostic.severity_value.empty() && diagnostic.severity_value != canonical) {
        throw std::invalid_argument("Invocation diagnostic severity value does not match severity");
    }
    (void)detail::canonical_json_bytes(diagnostic.witness);
}

json diagnostic_json(const InvocationDiagnostic& diagnostic) {
    validate_diagnostic(diagnostic);
    const auto severity = diagnostic.severity == InvocationDiagnosticSeverity::Unknown
                              ? diagnostic.severity_value
                              : std::string(to_string(diagnostic.severity));
    return json{{"code", diagnostic.code},
                {"message", diagnostic.message},
                {"severity", severity},
                {"blocking", diagnostic.blocking},
                {"witness", diagnostic.witness}};
}

InvocationDiagnostic diagnostic_from_json(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Invocation diagnostic must be an object");
    detail::reject_unknown_fields(value,
                                  "Invocation diagnostic",
                                  {"code", "message", "severity", "blocking", "witness"});
    InvocationDiagnostic diagnostic;
    diagnostic.code = required_string(value, "code");
    diagnostic.message = required_string(value, "message");
    diagnostic.severity_value = required_string(value, "severity");
    diagnostic.severity = invocation_diagnostic_severity_from_string(diagnostic.severity_value);
    diagnostic.blocking = required_value(value, "blocking").is_boolean()
                              ? required_value(value, "blocking").get<bool>()
                              : throw std::invalid_argument("Invocation diagnostic blocking must be boolean");
    diagnostic.witness = required_value(value, "witness");
    validate_diagnostic(diagnostic);
    return diagnostic;
}

json terminal_json(const InvocationTerminal& terminal) {
    std::string status_value = terminal.status_value;
    if (terminal.status != InvocationTerminalStatus::Unknown && status_value == "unknown") {
        status_value = std::string(to_string(terminal.status));
    }
    if (status_value.empty()) {
        throw std::invalid_argument("Invocation terminal status value must not be empty");
    }
    detail::validate_token(status_value, "Invocation terminal status");
    const auto parsed = invocation_terminal_status_from_string(status_value);
    if (parsed != terminal.status) {
        throw std::invalid_argument("Invocation terminal status does not match status value");
    }
    json diagnostics = json::array();
    for (const auto& diagnostic : terminal.diagnostics) diagnostics.push_back(diagnostic_json(diagnostic));
    (void)detail::canonical_json_bytes(terminal.output);
    return json{{"format", std::string(TERMINAL_FORMAT)},
                {"storage_schema_version", InvocationTerminal::STORAGE_SCHEMA_VERSION},
                {"status", status_value},
                {"diagnostics", diagnostics},
                {"output", terminal.output}};
}

InvocationTerminal terminal_from_json(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("Invocation terminal must be an object");
    detail::reject_unknown_fields(
        value, "Invocation terminal", {"format", "storage_schema_version", "status", "diagnostics", "output"});
    if (required_string(value, "format") != TERMINAL_FORMAT ||
        required_u32(value, "storage_schema_version") != InvocationTerminal::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported invocation terminal format or schema version");
    }
    InvocationTerminal terminal;
    terminal.status_value = required_string(value, "status");
    terminal.status = invocation_terminal_status_from_string(terminal.status_value);
    const auto& diagnostics = required_value(value, "diagnostics");
    if (!diagnostics.is_array()) throw std::invalid_argument("Invocation terminal diagnostics must be an array");
    for (const auto& diagnostic : diagnostics) terminal.diagnostics.push_back(diagnostic_from_json(diagnostic));
    terminal.output = required_value(value, "output");
    (void)terminal_json(terminal);
    return terminal;
}

RunInvocation invocation_from_json(const json& value) {
    if (!value.is_object()) throw std::invalid_argument("RunInvocation must be an object");
    detail::reject_unknown_fields(value,
                                  "Stored RunInvocation",
                                  {"format",
                                   "storage_schema_version",
                                   "id",
                                   "protocol_revision",
                                   "owner_scope",
                                   "agent_id",
                                   "program_version_id",
                                   "run_id",
                                   "parent_run_id",
                                   "budget",
                                   "input",
                                   "message_sequence",
                                   "idempotency_key",
                                   "correlation_id",
                                   "artifact"});
    if (required_string(value, "format") != INVOCATION_FORMAT ||
        required_u32(value, "storage_schema_version") != RunInvocation::STORAGE_SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported RunInvocation format or schema version");
    }
    RunInvocation invocation;
    invocation.protocol_revision = required_u32(value, "protocol_revision");
    invocation.owner_scope = required_string(value, "owner_scope");
    invocation.agent_id = required_string(value, "agent_id");
    invocation.program_version_id = required_string(value, "program_version_id");
    invocation.run_id = required_string(value, "run_id");
    invocation.parent_run_id = required_string(value, "parent_run_id");
    invocation.budget = decode_budget(required_value(value, "budget"));
    invocation.input = required_value(value, "input");
    invocation.message_sequence = required_unsigned(value, "message_sequence");
    invocation.idempotency_key = required_string(value, "idempotency_key");
    invocation.correlation_id = required_string(value, "correlation_id");
    const auto& artifact = required_value(value, "artifact");
    if (!artifact.is_null()) invocation.artifact = decode_artifact(artifact);
    invocation.validate();
    if (invocation.canonical_identity() != required_string(value, "id")) {
        throw std::invalid_argument("RunInvocation id does not match its canonical payload");
    }
    return invocation;
}

}  // namespace

std::string_view to_string(InvocationTerminalStatus status) noexcept {
    switch (status) {
        case InvocationTerminalStatus::Unknown:
            return "unknown";
        case InvocationTerminalStatus::Completed:
            return "completed";
        case InvocationTerminalStatus::Failed:
            return "failed";
        case InvocationTerminalStatus::Interrupted:
            return "interrupted";
        case InvocationTerminalStatus::Cancelled:
            return "cancelled";
        case InvocationTerminalStatus::BudgetExhausted:
            return "budget_exhausted";
        case InvocationTerminalStatus::TimedOut:
            return "timed_out";
    }
    return "unknown";
}

InvocationTerminalStatus invocation_terminal_status_from_string(std::string_view value) noexcept {
    if (value == "completed") return InvocationTerminalStatus::Completed;
    if (value == "failed") return InvocationTerminalStatus::Failed;
    if (value == "interrupted") return InvocationTerminalStatus::Interrupted;
    if (value == "cancelled") return InvocationTerminalStatus::Cancelled;
    if (value == "budget_exhausted") return InvocationTerminalStatus::BudgetExhausted;
    if (value == "timed_out") return InvocationTerminalStatus::TimedOut;
    return InvocationTerminalStatus::Unknown;
}

bool invocation_terminal_status_succeeded(InvocationTerminalStatus status) noexcept {
    return status == InvocationTerminalStatus::Completed;
}

bool invocation_terminal_status_failed(InvocationTerminalStatus status) noexcept {
    return status != InvocationTerminalStatus::Completed;
}

std::string_view to_string(InvocationDiagnosticSeverity severity) noexcept {
    switch (severity) {
        case InvocationDiagnosticSeverity::Unknown:
            return "unknown";
        case InvocationDiagnosticSeverity::Error:
            return "error";
        case InvocationDiagnosticSeverity::Warning:
            return "warning";
        case InvocationDiagnosticSeverity::Info:
            return "info";
    }
    return "unknown";
}

InvocationDiagnosticSeverity invocation_diagnostic_severity_from_string(std::string_view value) noexcept {
    if (value == "error") return InvocationDiagnosticSeverity::Error;
    if (value == "warning") return InvocationDiagnosticSeverity::Warning;
    if (value == "info") return InvocationDiagnosticSeverity::Info;
    return InvocationDiagnosticSeverity::Unknown;
}

InvocationTerminal InvocationTerminal::create(std::string value,
                                              json output,
                                              std::vector<InvocationDiagnostic> diagnostics) {
    InvocationTerminal terminal;
    terminal.status_value = std::move(value);
    terminal.status = invocation_terminal_status_from_string(terminal.status_value);
    terminal.output = std::move(output);
    terminal.diagnostics = std::move(diagnostics);
    (void)terminal_json(terminal);
    return terminal;
}

InvocationTerminal InvocationTerminal::parse(std::string_view stored_bytes) {
    try {
        return terminal_from_json(detail::parse_json_strict(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored InvocationTerminal JSON: ") + error.what());
    }
}

bool InvocationTerminal::succeeded() const noexcept {
    return status == InvocationTerminalStatus::Completed &&
           (status_value.empty() || status_value == "unknown" || status_value == "completed");
}

bool InvocationTerminal::failed() const noexcept {
    return !succeeded();
}

std::string InvocationTerminal::serialize_canonical() const {
    return detail::canonical_json_bytes(terminal_json(*this));
}

void to_json(json& value, const InvocationDiagnostic& diagnostic) { value = diagnostic_json(diagnostic); }

void from_json(const json& value, InvocationDiagnostic& diagnostic) {
    diagnostic = diagnostic_from_json(value);
}

void to_json(json& value, const InvocationTerminal& terminal) { value = terminal_json(terminal); }
void from_json(const json& value, InvocationTerminal& terminal) {
    terminal = terminal_from_json(value);
}

void to_json(json& value, const InvocationArtifactReference& artifact) {
    detail::validate_token(artifact.artifact_identity, "Invocation artifact_identity");
    detail::validate_token(artifact.uri, "Invocation artifact uri");
    detail::validate_token(artifact.media_type, "Invocation artifact media_type");
    value = encode_artifact(artifact);
}

void from_json(const json& value, InvocationArtifactReference& artifact) {
    artifact = decode_artifact(value);
    detail::validate_token(artifact.artifact_identity, "Invocation artifact_identity");
    detail::validate_token(artifact.uri, "Invocation artifact uri");
    detail::validate_token(artifact.media_type, "Invocation artifact media_type");
}

void to_json(json& value, const RunInvocation& invocation) {
    value = detail::parse_json_strict(invocation.serialize_canonical());
}

void from_json(const json& value, RunInvocation& invocation) {
    invocation = invocation_from_json(value);
}


void RunInvocation::validate() const {
    if (protocol_revision != PROTOCOL_REVISION) {
        throw std::invalid_argument("Unsupported RunInvocation protocol revision");
    }
    detail::validate_token(owner_scope, "RunInvocation owner_scope");
    detail::validate_token(agent_id, "RunInvocation agent_id");
    detail::validate_token(program_version_id, "RunInvocation program_version_id");
    detail::validate_token(run_id, "RunInvocation run_id");
    if (!parent_run_id.empty()) detail::validate_token(parent_run_id, "RunInvocation parent_run_id");
    if (message_sequence == 0) {
        throw std::invalid_argument("RunInvocation message_sequence must start at one");
    }
    detail::validate_token(idempotency_key, "RunInvocation idempotency_key");
    detail::validate_token(correlation_id, "RunInvocation correlation_id");
    (void)detail::canonical_json_bytes(input);
    if (artifact) {
        detail::validate_token(artifact->artifact_identity, "RunInvocation artifact_identity");
        detail::validate_token(artifact->uri, "RunInvocation artifact uri");
        detail::validate_token(artifact->media_type, "RunInvocation artifact media_type");
    }
}

std::string RunInvocation::canonical_payload() const {
    validate();
    return detail::canonical_json_bytes(invocation_payload(*this));
}

std::string RunInvocation::canonical_identity() const {
    return detail::sha256_identity("program-run-invocation/v1", canonical_payload());
}

std::string RunInvocation::serialize_canonical() const {
    const auto payload = invocation_payload(*this);
    const auto identity = canonical_identity();
    json value{{"format", std::string(INVOCATION_FORMAT)},
               {"storage_schema_version", STORAGE_SCHEMA_VERSION},
               {"id", identity},
               {"protocol_revision", protocol_revision},
               {"owner_scope", owner_scope},
               {"agent_id", agent_id},
               {"program_version_id", program_version_id},
               {"run_id", run_id},
               {"parent_run_id", parent_run_id},
               {"budget", payload["budget"]},
               {"input", input},
               {"message_sequence", message_sequence},
               {"idempotency_key", idempotency_key},
               {"correlation_id", correlation_id},
               {"artifact", artifact ? encode_artifact(*artifact) : json(nullptr)}};
    return detail::canonical_json_bytes(value);
}

RunInvocation RunInvocation::parse(std::string_view stored_bytes) {
    try {
        return invocation_from_json(detail::parse_json_strict(stored_bytes));
    } catch (const std::exception& error) {
        throw std::invalid_argument(std::string("Invalid stored RunInvocation JSON: ") + error.what());
    }
}


bool RunInvocation::operator==(const RunInvocation& other) const {
    return idempotency_key == other.idempotency_key &&
           canonical_payload() == other.canonical_payload();
}
std::string_view to_string(InvocationIdempotencyResult result) noexcept {
    switch (result) {
        case InvocationIdempotencyResult::Distinct:
            return "distinct";
        case InvocationIdempotencyResult::Duplicate:
            return "duplicate";
        case InvocationIdempotencyResult::Conflict:
            return "conflict";
    }
    return "conflict";
}

InvocationIdempotencyResult compare_invocations(const RunInvocation& existing,
                                                const RunInvocation& incoming) {
    if (existing.idempotency_key.empty() || incoming.idempotency_key.empty() ||
        existing.idempotency_key != incoming.idempotency_key) {
        return InvocationIdempotencyResult::Distinct;
    }
    return existing.canonical_payload() == incoming.canonical_payload()
               ? InvocationIdempotencyResult::Duplicate
               : InvocationIdempotencyResult::Conflict;
}

InvocationIdempotencyResult compare_invocation_idempotency(const RunInvocation& existing,
                                                           const RunInvocation& incoming) {
    return compare_invocations(existing, incoming);
}

}  // namespace neograph::program
