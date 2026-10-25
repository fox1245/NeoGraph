#include <neograph/program/gate.h>

#include "../core/sha256.h"
#include "canonical_json.h"
#include "javascript_pure.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

using detail::canonical_json_bytes;
using detail::parse_json_strict;
using detail::sha256_identity;
using detail::validate_token;
using detail::validate_utf8;

constexpr std::string_view kGateSlotFormat                 = "neograph-program-gate-slot";
constexpr std::string_view kGateVersionFormat              = "neograph-program-gate-version";
constexpr std::string_view kGateVerdictTokenFormat         = "neograph-program-gate-verdict-token";
constexpr std::string_view kGateQualificationReceiptFormat = "neograph-program-gate-qualification-receipt";

constexpr std::string_view kGateSlotDomain                 = "program-gate-test-slot/v1";
constexpr std::string_view kGatePublicSlotDomain           = "program-gate-public-slot/v1";
constexpr std::string_view kGateVersionDomain              = "program-gate-version/v1";
constexpr std::string_view kGateValueDomain                = "program-gate-value/v1";
constexpr std::string_view kGateVerdictTokenDomain         = "program-gate-verdict-token/v1";
constexpr std::string_view kGateQualificationMutantsDomain = "program-gate-qualification-mutants/v1";
constexpr std::string_view kGateQualificationReceiptDomain = "program-gate-qualification-receipt/v1";

[[noreturn]] void throw_gate_diagnostic(std::string_view source_id,
                                        std::string_view code,
                                        std::string      message,
                                        json             witness = json::object()) {
    Diagnostic diagnostic;
    diagnostic.phase                = CompilePhase::Source;
    diagnostic.code                 = std::string(code);
    diagnostic.severity             = DiagnosticSeverity::Error;
    diagnostic.primary.source_id    = source_id.empty() ? std::string("<gate>") : std::string(source_id);
    diagnostic.primary.json_pointer = "";
    diagnostic.message              = std::move(message);
    diagnostic.witness              = std::move(witness);
    throw ProgramDiagnosticError(std::move(diagnostic));
}

std::string string_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_string()) {
        throw std::invalid_argument("Gate field '" + key + "' must be a string");
    }
    return object[key].get<std::string>();
}

std::uint64_t unsigned_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_number_unsigned()) {
        throw std::invalid_argument("Gate field '" + key + "' must be an unsigned integer");
    }
    return object[key].get<std::uint64_t>();
}

bool bool_value(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || !object[key].is_boolean()) {
        throw std::invalid_argument("Gate field '" + key + "' must be a boolean");
    }
    return object[key].get<bool>();
}

json required_field(const json& object, std::string_view field) {
    const auto key = std::string(field);
    if (!object.contains(key) || object[key].is_null()) {
        throw std::invalid_argument("Gate field '" + key + "' is required and must not be null");
    }
    return object[key];
}

json required_object(const json& object, std::string_view field) {
    const auto value = required_field(object, field);
    if (!value.is_object()) {
        throw std::invalid_argument("Gate field '" + std::string(field) + "' must be an object");
    }
    return value;
}

json required_array(const json& object, std::string_view field) {
    const auto value = required_field(object, field);
    if (!value.is_array()) {
        throw std::invalid_argument("Gate field '" + std::string(field) + "' must be an array");
    }
    return value;
}

void validate_identifier(std::string_view value, std::string_view field) {
    if (value.empty()) {
        throw std::invalid_argument("Gate field '" + std::string(field) + "' must not be empty");
    }
    validate_utf8(value);
    validate_token(value, field);
}

void validate_nonempty_text(std::string_view value, std::string_view field) {
    if (value.empty()) {
        throw std::invalid_argument("Gate field '" + std::string(field) + "' must not be empty");
    }
    validate_utf8(value);
}

void validate_sandbox_limits(const GateSandboxLimits& limits) {
    if (limits.case_wall_time_ms == 0 || limits.case_interrupt_polls == 0 ||
        limits.case_memory_limit_bytes == 0 || limits.case_max_stack_bytes == 0) {
        throw std::invalid_argument("GateSandboxLimits fields must all be positive");
    }
}

void validate_feedback_policy(const GateFeedbackPolicy& policy) {
    switch (policy.mode) {
        case GateFeedbackMode::VerdictOnly:
        case GateFeedbackMode::FailedCount:
        case GateFeedbackMode::PublicExample:
        case GateFeedbackMode::SanitizedSymptoms:
            return;
    }
    throw std::invalid_argument("Invalid GateFeedbackMode");
}

}  // namespace

std::string_view to_string(GateFeedbackMode mode) noexcept {
    switch (mode) {
        case GateFeedbackMode::VerdictOnly:
            return "verdict_only";
        case GateFeedbackMode::FailedCount:
            return "failed_count";
        case GateFeedbackMode::PublicExample:
            return "public_example";
        case GateFeedbackMode::SanitizedSymptoms:
            return "sanitized_symptoms";
    }
    return "unknown";
}

GateFeedbackMode gate_feedback_mode_from_string(std::string_view value) {
    if (value == "verdict_only") return GateFeedbackMode::VerdictOnly;
    if (value == "failed_count") return GateFeedbackMode::FailedCount;
    if (value == "public_example") return GateFeedbackMode::PublicExample;
    if (value == "sanitized_symptoms") return GateFeedbackMode::SanitizedSymptoms;
    throw std::invalid_argument("Unknown gate feedback mode: " + std::string(value));
}

std::string_view to_string(GateFailureCategory category) noexcept {
    switch (category) {
        case GateFailureCategory::Timeout:
            return "timeout";
        case GateFailureCategory::MemoryLimit:
            return "memory_limit";
        case GateFailureCategory::SchemaMismatch:
            return "schema_mismatch";
        case GateFailureCategory::AssertionFailed:
            return "assertion_failed";
        case GateFailureCategory::RuntimeException:
            return "runtime_exception";
    }
    return "unknown";
}

GateFailureCategory gate_failure_category_from_string(std::string_view value) {
    if (value == "timeout") return GateFailureCategory::Timeout;
    if (value == "memory_limit") return GateFailureCategory::MemoryLimit;
    if (value == "schema_mismatch") return GateFailureCategory::SchemaMismatch;
    if (value == "assertion_failed") return GateFailureCategory::AssertionFailed;
    if (value == "runtime_exception") return GateFailureCategory::RuntimeException;
    throw std::invalid_argument("Unknown gate failure category: " + std::string(value));
}

void to_json(json& value, const GatePublicExample& example) {
    value = json{{"case_id", example.case_id},
                 {"input", example.input},
                 {"expected", example.expected}};
}

void from_json(const json& value, GatePublicExample& example) {
    detail::reject_unknown_fields(value, "gate.public_example", {"case_id", "input", "expected"});
    example.case_id  = string_value(value, "case_id");
    example.input    = required_field(value, "input");
    example.expected = required_field(value, "expected");
}

void to_json(json& value, const GateHiddenCase& hidden_case) {
    const bool has_expected = hidden_case.expected.has_value();
    const bool has_property = hidden_case.property_entrypoint.has_value();
    if (has_expected == has_property) {
        throw std::invalid_argument(
            "GateHiddenCase must specify exactly one of expected or property_entrypoint");
    }
    value = json{{"case_id", hidden_case.case_id}, {"input", hidden_case.input}};
    if (has_expected) {
        value["expected"] = *hidden_case.expected;
    } else {
        value["property_entrypoint"] = *hidden_case.property_entrypoint;
    }
}

void from_json(const json& value, GateHiddenCase& hidden_case) {
    detail::reject_unknown_fields(
        value, "gate.hidden_case", {"case_id", "input", "expected", "property_entrypoint"});
    const bool has_expected = value.contains("expected");
    const bool has_property = value.contains("property_entrypoint");
    if (has_expected == has_property) {
        throw std::invalid_argument(
            "GateHiddenCase must specify exactly one of expected or property_entrypoint");
    }
    hidden_case.case_id = string_value(value, "case_id");
    hidden_case.input   = required_field(value, "input");
    hidden_case.expected.reset();
    hidden_case.property_entrypoint.reset();
    if (has_expected) {
        hidden_case.expected = required_field(value, "expected");
    } else {
        hidden_case.property_entrypoint = string_value(value, "property_entrypoint");
    }
}

void to_json(json& value, const GatePropertyModule& module) {
    value = json{{"source", module.source}};
}

void from_json(const json& value, GatePropertyModule& module) {
    detail::reject_unknown_fields(value, "gate.property_module", {"source"});
    module.source = string_value(value, "source");
}

void to_json(json& value, const GateCaseGenerator& generator) {
    value = json{{"source", generator.source},
                 {"generated_case_count", generator.generated_case_count},
                 {"property_entrypoint", generator.property_entrypoint}};
}

void from_json(const json& value, GateCaseGenerator& generator) {
    detail::reject_unknown_fields(
        value,
        "gate.case_generator",
        {"source", "generated_case_count", "property_entrypoint"});
    generator.source               = string_value(value, "source");
    generator.generated_case_count = unsigned_value(value, "generated_case_count");
    generator.property_entrypoint  = string_value(value, "property_entrypoint");
}

void to_json(json& value, const GateQualificationMutant& mutant) {
    value = json{{"mutant_id", mutant.mutant_id}, {"source", mutant.source}};
}

void from_json(const json& value, GateQualificationMutant& mutant) {
    detail::reject_unknown_fields(value, "gate.qualification_mutant", {"mutant_id", "source"});
    mutant.mutant_id = string_value(value, "mutant_id");
    mutant.source    = string_value(value, "source");
}

void to_json(json& value, const GateGoldenCandidate& candidate) {
    value = json{{"candidate_id", candidate.candidate_id}, {"source", candidate.source}};
}

void from_json(const json& value, GateGoldenCandidate& candidate) {
    detail::reject_unknown_fields(value, "gate.golden_candidate", {"candidate_id", "source"});
    candidate.candidate_id = string_value(value, "candidate_id");
    candidate.source       = string_value(value, "source");
}

void to_json(json& value, const GateSandboxLimits& limits) {
    validate_sandbox_limits(limits);
    value = json{{"case_wall_time_ms", limits.case_wall_time_ms},
                 {"case_interrupt_polls", limits.case_interrupt_polls},
                 {"case_memory_limit_bytes", limits.case_memory_limit_bytes},
                 {"case_max_stack_bytes", limits.case_max_stack_bytes}};
}

void from_json(const json& value, GateSandboxLimits& limits) {
    detail::reject_unknown_fields(value,
                                  "gate.sandbox_limits",
                                  {"case_wall_time_ms",
                                   "case_interrupt_polls",
                                   "case_memory_limit_bytes",
                                   "case_max_stack_bytes"});
    limits.case_wall_time_ms       = unsigned_value(value, "case_wall_time_ms");
    limits.case_interrupt_polls    = unsigned_value(value, "case_interrupt_polls");
    limits.case_memory_limit_bytes =
        static_cast<std::size_t>(unsigned_value(value, "case_memory_limit_bytes"));
    limits.case_max_stack_bytes =
        static_cast<std::size_t>(unsigned_value(value, "case_max_stack_bytes"));
    validate_sandbox_limits(limits);
}

void to_json(json& value, const GateFeedbackPolicy& policy) {
    validate_feedback_policy(policy);
    value = json{{"mode", std::string(to_string(policy.mode))}};
}

void from_json(const json& value, GateFeedbackPolicy& policy) {
    detail::reject_unknown_fields(value, "gate.feedback_policy", {"mode"});
    policy.mode = gate_feedback_mode_from_string(string_value(value, "mode"));
}

void to_json(json& value, const GateVerdict& verdict) {
    json categories = json::array();
    for (const auto& category : verdict.failure_categories) {
        (void)gate_failure_category_from_string(category);
        categories.push_back(category);
    }
    value = json{{"passed", verdict.passed},
                 {"total_cases", verdict.total_cases},
                 {"failed_cases", verdict.failed_cases},
                 {"public_feedback", verdict.public_feedback},
                 {"failure_categories", std::move(categories)}};
}

void from_json(const json& value, GateVerdict& verdict) {
    detail::reject_unknown_fields(value,
                                  "gate.verdict",
                                  {"passed",
                                   "total_cases",
                                   "failed_cases",
                                   "public_feedback",
                                   "failure_categories"});
    verdict.passed          = bool_value(value, "passed");
    verdict.total_cases     = unsigned_value(value, "total_cases");
    verdict.failed_cases    = unsigned_value(value, "failed_cases");
    verdict.public_feedback = required_object(value, "public_feedback");
    verdict.failure_categories.clear();
    for (const auto& item : required_array(value, "failure_categories")) {
        if (!item.is_string()) {
            throw std::invalid_argument("GateVerdict failure_categories must contain strings");
        }
        const auto category = item.get<std::string>();
        (void)gate_failure_category_from_string(category);
        verdict.failure_categories.push_back(category);
    }
}

namespace {

void validate_slot_data(const GateTestSlotData& data) {
    validate_identifier(data.slot_id, "slot_id");
    validate_identifier(data.owner_scope, "owner_scope");

    std::set<std::string> case_ids;
    for (const auto& example : data.public_examples) {
        validate_identifier(example.case_id, "public_examples.case_id");
        if (!case_ids.insert(example.case_id).second) {
            throw std::invalid_argument("Duplicate gate test slot case_id: " + example.case_id);
        }
        if (example.input.is_null() || example.expected.is_null()) {
            throw std::invalid_argument("GatePublicExample input and expected must not be null");
        }
        (void)canonical_json_bytes(example.input);
        (void)canonical_json_bytes(example.expected);
    }

    bool requires_property_module = false;
    for (const auto& hidden : data.hidden_cases) {
        validate_identifier(hidden.case_id, "hidden_cases.case_id");
        if (!case_ids.insert(hidden.case_id).second) {
            throw std::invalid_argument("Duplicate gate test slot case_id: " + hidden.case_id);
        }
        if (hidden.input.is_null()) {
            throw std::invalid_argument("GateHiddenCase input must not be null");
        }
        (void)canonical_json_bytes(hidden.input);
        const bool has_expected = hidden.expected.has_value();
        const bool has_property = hidden.property_entrypoint.has_value();
        if (has_expected == has_property) {
            throw std::invalid_argument(
                "GateHiddenCase must specify exactly one of expected or property_entrypoint");
        }
        if (has_expected) {
            if (hidden.expected->is_null()) {
                throw std::invalid_argument("GateHiddenCase expected must not be null");
            }
            (void)canonical_json_bytes(*hidden.expected);
        } else {
            validate_identifier(*hidden.property_entrypoint, "hidden_cases.property_entrypoint");
            requires_property_module = true;
        }
    }

    if (data.property_module.has_value()) {
        validate_nonempty_text(data.property_module->source, "property_module.source");
    }

    if (data.case_generator.has_value()) {
        validate_nonempty_text(data.case_generator->source, "case_generator.source");
        if (data.case_generator->generated_case_count == 0) {
            throw std::invalid_argument(
                "GateCaseGenerator generated_case_count must be positive");
        }
        validate_identifier(data.case_generator->property_entrypoint,
                            "case_generator.property_entrypoint");
        requires_property_module = true;
    }

    if (requires_property_module && !data.property_module.has_value()) {
        throw std::invalid_argument(
            "GateTestSlot requires property_module when property_entrypoint is used");
    }

    const bool has_generated =
        data.case_generator.has_value() && data.case_generator->generated_case_count > 0;
    if (data.public_examples.empty() && data.hidden_cases.empty() && !has_generated) {
        throw std::invalid_argument("GateTestSlot must declare at least one test case");
    }

    std::set<std::string> mutant_ids;
    for (const auto& mutant : data.qualification_mutants) {
        validate_identifier(mutant.mutant_id, "qualification_mutants.mutant_id");
        if (!mutant_ids.insert(mutant.mutant_id).second) {
            throw std::invalid_argument("Duplicate qualification mutant_id: " + mutant.mutant_id);
        }
        validate_nonempty_text(mutant.source, "qualification_mutants.source");
    }

    std::set<std::string> golden_ids;
    for (const auto& candidate : data.golden_candidates) {
        validate_identifier(candidate.candidate_id, "golden_candidates.candidate_id");
        if (!golden_ids.insert(candidate.candidate_id).second) {
            throw std::invalid_argument("Duplicate golden candidate_id: " + candidate.candidate_id);
        }
        validate_nonempty_text(candidate.source, "golden_candidates.source");
    }
}

json slot_to_json(const GateTestSlotData& data) {
    json public_examples = json::array();
    for (const auto& example : data.public_examples) {
        json encoded;
        to_json(encoded, example);
        public_examples.push_back(std::move(encoded));
    }
    json hidden_cases = json::array();
    for (const auto& hidden : data.hidden_cases) {
        json encoded;
        to_json(encoded, hidden);
        hidden_cases.push_back(std::move(encoded));
    }
    json mutants = json::array();
    for (const auto& mutant : data.qualification_mutants) {
        json encoded;
        to_json(encoded, mutant);
        mutants.push_back(std::move(encoded));
    }
    json goldens = json::array();
    for (const auto& golden : data.golden_candidates) {
        json encoded;
        to_json(encoded, golden);
        goldens.push_back(std::move(encoded));
    }

    json result{{"format", kGateSlotFormat},
                {"schema_version", GateTestSlot::SCHEMA_VERSION},
                {"slot_id", data.slot_id},
                {"owner_scope", data.owner_scope},
                {"public_examples", std::move(public_examples)},
                {"hidden_cases", std::move(hidden_cases)},
                {"qualification_mutants", std::move(mutants)},
                {"golden_candidates", std::move(goldens)}};
    if (data.property_module.has_value()) {
        json encoded;
        to_json(encoded, *data.property_module);
        result["property_module"] = std::move(encoded);
    }
    if (data.case_generator.has_value()) {
        json encoded;
        to_json(encoded, *data.case_generator);
        result["case_generator"] = std::move(encoded);
    }
    return result;
}

json slot_public_to_json(const GateTestSlotData& data) {
    json public_examples = json::array();
    for (const auto& example : data.public_examples) {
        json encoded;
        to_json(encoded, example);
        public_examples.push_back(std::move(encoded));
    }
    return json{{"public_examples", std::move(public_examples)},
                {"schema_version", GateTestSlot::SCHEMA_VERSION}};
}

GateTestSlotData slot_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "gate.test_slot",
                                  {"format",
                                   "schema_version",
                                   "slot_id",
                                   "owner_scope",
                                   "public_examples",
                                   "hidden_cases",
                                   "property_module",
                                   "case_generator",
                                   "qualification_mutants",
                                   "golden_candidates"});
    if (string_value(value, "format") != kGateSlotFormat) {
        throw std::invalid_argument("Unsupported gate test slot format");
    }
    if (unsigned_value(value, "schema_version") != GateTestSlot::SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported gate test slot schema_version");
    }

    GateTestSlotData data;
    data.slot_id     = string_value(value, "slot_id");
    data.owner_scope = string_value(value, "owner_scope");

    for (const auto& item : required_array(value, "public_examples")) {
        GatePublicExample example;
        from_json(item, example);
        data.public_examples.push_back(std::move(example));
    }
    for (const auto& item : required_array(value, "hidden_cases")) {
        GateHiddenCase hidden;
        from_json(item, hidden);
        data.hidden_cases.push_back(std::move(hidden));
    }
    if (value.contains("property_module")) {
        GatePropertyModule module;
        from_json(required_object(value, "property_module"), module);
        data.property_module = std::move(module);
    }
    if (value.contains("case_generator")) {
        GateCaseGenerator generator;
        from_json(required_object(value, "case_generator"), generator);
        data.case_generator = std::move(generator);
    }
    for (const auto& item : required_array(value, "qualification_mutants")) {
        GateQualificationMutant mutant;
        from_json(item, mutant);
        data.qualification_mutants.push_back(std::move(mutant));
    }
    for (const auto& item : required_array(value, "golden_candidates")) {
        GateGoldenCandidate golden;
        from_json(item, golden);
        data.golden_candidates.push_back(std::move(golden));
    }
    return data;
}

void validate_gate_data(const ProgramGateData& data) {
    validate_identifier(data.owner_scope, "owner_scope");
    validate_identifier(data.gate_name, "gate_name");
    validate_identifier(data.test_slot_digest, "test_slot_digest");
    validate_sandbox_limits(data.sandbox_limits);
    validate_feedback_policy(data.feedback_policy);
}

json gate_version_to_json(const ProgramGateData& data) {
    json limits_json;
    to_json(limits_json, data.sandbox_limits);
    json policy_json;
    to_json(policy_json, data.feedback_policy);
    return json{{"format", kGateVersionFormat},
                {"schema_version", ProgramGateVersion::SCHEMA_VERSION},
                {"owner_scope", data.owner_scope},
                {"gate_name", data.gate_name},
                {"test_slot_digest", data.test_slot_digest},
                {"sandbox_limits", std::move(limits_json)},
                {"feedback_policy", std::move(policy_json)}};
}

ProgramGateData gate_version_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "gate.version",
                                  {"format",
                                   "schema_version",
                                   "owner_scope",
                                   "gate_name",
                                   "test_slot_digest",
                                   "sandbox_limits",
                                   "feedback_policy"});
    if (string_value(value, "format") != kGateVersionFormat) {
        throw std::invalid_argument("Unsupported program gate version format");
    }
    if (unsigned_value(value, "schema_version") != ProgramGateVersion::SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported program gate version schema_version");
    }
    GateSandboxLimits limits{};
    from_json(required_object(value, "sandbox_limits"), limits);
    GateFeedbackPolicy policy{};
    from_json(required_object(value, "feedback_policy"), policy);
    return ProgramGateData{string_value(value, "owner_scope"),
                           string_value(value, "gate_name"),
                           string_value(value, "test_slot_digest"),
                           limits,
                           policy};
}

void validate_token_data(const GateVerdictTokenData& data) {
    validate_identifier(data.owner_scope, "owner_scope");
    validate_identifier(data.subject_id, "subject_id");
    validate_identifier(data.gate_version_id, "gate_version_id");
    validate_identifier(data.test_slot_digest, "test_slot_digest");
    validate_identifier(data.checked_value_digest, "checked_value_digest");
    validate_utf8(data.generator_seed_hex);
    if (!data.generator_seed_hex.empty()) {
        validate_token(data.generator_seed_hex, "generator_seed_hex");
    }
}

json token_to_json(const GateVerdictTokenData& data) {
    return json{{"format", kGateVerdictTokenFormat},
                {"schema_version", GateVerdictToken::SCHEMA_VERSION},
                {"owner_scope", data.owner_scope},
                {"subject_id", data.subject_id},
                {"gate_version_id", data.gate_version_id},
                {"test_slot_digest", data.test_slot_digest},
                {"checked_value_digest", data.checked_value_digest},
                {"attempt", data.attempt},
                {"generator_seed_hex", data.generator_seed_hex}};
}

GateVerdictTokenData token_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "gate.verdict_token",
                                  {"format",
                                   "schema_version",
                                   "owner_scope",
                                   "subject_id",
                                   "gate_version_id",
                                   "test_slot_digest",
                                   "checked_value_digest",
                                   "attempt",
                                   "generator_seed_hex"});
    if (string_value(value, "format") != kGateVerdictTokenFormat) {
        throw std::invalid_argument("Unsupported gate verdict token format");
    }
    if (unsigned_value(value, "schema_version") != GateVerdictToken::SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported gate verdict token schema_version");
    }
    return GateVerdictTokenData{string_value(value, "owner_scope"),
                                string_value(value, "subject_id"),
                                string_value(value, "gate_version_id"),
                                string_value(value, "test_slot_digest"),
                                string_value(value, "checked_value_digest"),
                                unsigned_value(value, "attempt"),
                                string_value(value, "generator_seed_hex")};
}

void validate_receipt_data(const GateSlotQualificationReceiptData& data) {
    validate_identifier(data.gate_version_id, "gate_version_id");
    validate_identifier(data.test_slot_digest, "test_slot_digest");
    validate_identifier(data.mutants_digest, "mutants_digest");
    validate_identifier(data.generator_seed_hex, "generator_seed_hex");
    if (data.total_mutants == 0) {
        throw std::invalid_argument(
            "GateSlotQualificationReceipt total_mutants must be positive");
    }
    if (data.killed_mutants > data.total_mutants) {
        throw std::invalid_argument(
            "GateSlotQualificationReceipt killed_mutants cannot exceed total_mutants");
    }
}

json receipt_to_json(const GateSlotQualificationReceiptData& data) {
    return json{{"format", kGateQualificationReceiptFormat},
                {"schema_version", GateSlotQualificationReceipt::SCHEMA_VERSION},
                {"gate_version_id", data.gate_version_id},
                {"test_slot_digest", data.test_slot_digest},
                {"mutants_digest", data.mutants_digest},
                {"total_mutants", data.total_mutants},
                {"killed_mutants", data.killed_mutants},
                {"golden_passed", data.golden_passed},
                {"generator_seed_hex", data.generator_seed_hex}};
}

GateSlotQualificationReceiptData receipt_from_json(const json& value) {
    detail::reject_unknown_fields(value,
                                  "gate.qualification_receipt",
                                  {"format",
                                   "schema_version",
                                   "gate_version_id",
                                   "test_slot_digest",
                                   "mutants_digest",
                                   "total_mutants",
                                   "killed_mutants",
                                   "golden_passed",
                                   "generator_seed_hex"});
    if (string_value(value, "format") != kGateQualificationReceiptFormat) {
        throw std::invalid_argument("Unsupported gate qualification receipt format");
    }
    if (unsigned_value(value, "schema_version") != GateSlotQualificationReceipt::SCHEMA_VERSION) {
        throw std::invalid_argument("Unsupported gate qualification receipt schema_version");
    }
    return GateSlotQualificationReceiptData{string_value(value, "gate_version_id"),
                                            string_value(value, "test_slot_digest"),
                                            string_value(value, "mutants_digest"),
                                            unsigned_value(value, "total_mutants"),
                                            unsigned_value(value, "killed_mutants"),
                                            bool_value(value, "golden_passed"),
                                            string_value(value, "generator_seed_hex")};
}

std::string compute_mutants_digest(const std::vector<GateQualificationMutant>& mutants) {
    json array = json::array();
    for (const auto& mutant : mutants) {
        json encoded;
        to_json(encoded, mutant);
        array.push_back(std::move(encoded));
    }
    return sha256_identity(kGateQualificationMutantsDomain, canonical_json_bytes(array));
}

std::string default_qualification_seed(const GateTestSlot& slot) {
    constexpr char kHexDigits[] = "0123456789abcdef";
    const auto     digest       = neograph::detail::sha256_digest(slot.serialize_canonical());
    std::string    hex;
    hex.reserve(digest.size() * 2);
    for (const std::uint8_t byte : digest) {
        hex.push_back(kHexDigits[(byte >> 4) & 0x0fu]);
        hex.push_back(kHexDigits[byte & 0x0fu]);
    }
    return hex;
}

void record_failure_category(std::vector<std::string>& categories, GateFailureCategory category) {
    const std::string name(to_string(category));
    if (std::find(categories.begin(), categories.end(), name) == categories.end()) {
        categories.push_back(name);
    }
}

json build_public_feedback(GateFeedbackMode                   mode,
                           bool                               passed,
                           std::uint64_t                      total_cases,
                           std::uint64_t                      failed_cases,
                           const std::optional<json>&         first_failed_public_example,
                           const std::vector<std::string>&    failure_categories) {
    json feedback{{"passed", passed}, {"code", passed ? "GATE_PASSED" : "GATE_FAILED"}};
    if (mode == GateFeedbackMode::VerdictOnly) {
        return feedback;
    }
    if (mode == GateFeedbackMode::FailedCount) {
        feedback["total_cases"]  = total_cases;
        feedback["failed_cases"] = failed_cases;
        return feedback;
    }
    if (mode == GateFeedbackMode::PublicExample) {
        feedback["public_example"] =
            first_failed_public_example.has_value() ? *first_failed_public_example : json(nullptr);
        return feedback;
    }
    json categories = json::array();
    for (const auto& category : failure_categories) {
        categories.push_back(category);
    }
    feedback["failure_categories"] = std::move(categories);
    return feedback;
}

std::string extract_javascript_source_text(const ProgramSource& candidate) {
    if (candidate.kind() != SourceKind::JavaScript) {
        throw_gate_diagnostic(
            candidate.source_id(),
            "P_GATE_CANDIDATE_KIND_UNSUPPORTED",
            "Program gate candidate must be a JavaScript module source");
    }
    const auto doc = candidate.document();
    if (!doc.is_object() || !doc.contains("source") || !doc["source"].is_string()) {
        throw_gate_diagnostic(
            candidate.source_id(),
            "P_GATE_CANDIDATE_KIND_UNSUPPORTED",
            "Program gate candidate document is missing JavaScript source text");
    }
    return doc["source"].get<std::string>();
}

}  // namespace

struct GateTestSlot::Impl {
    GateTestSlotData data;
    std::string      canonical_bytes;
    std::string      digest;
    std::string      public_digest;
};

GateTestSlot::GateTestSlot(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

GateTestSlot GateTestSlot::create(GateTestSlotData data) {
    validate_slot_data(data);
    auto impl             = std::make_shared<Impl>();
    impl->canonical_bytes = canonical_json_bytes(slot_to_json(data));
    impl->digest          = sha256_identity(kGateSlotDomain, impl->canonical_bytes);
    impl->public_digest   =
        sha256_identity(kGatePublicSlotDomain, canonical_json_bytes(slot_public_to_json(data)));
    impl->data = std::move(data);
    return GateTestSlot(std::move(impl));
}

GateTestSlot GateTestSlot::parse(std::string_view canonical_bytes) {
    const auto parsed = parse_json_strict(canonical_bytes);
    auto       slot   = create(slot_from_json(parsed));
    if (slot.serialize_canonical() != canonical_json_bytes(parsed)) {
        throw std::invalid_argument(
            "GateTestSlot canonical serialization does not match parsed document");
    }
    return slot;
}

const GateTestSlotData& GateTestSlot::data() const noexcept {
    return impl_->data;
}

const std::string& GateTestSlot::digest() const noexcept {
    return impl_->digest;
}

const std::string& GateTestSlot::id() const noexcept {
    return impl_->digest;
}

const std::string& GateTestSlot::public_digest() const noexcept {
    return impl_->public_digest;
}

const std::string& GateTestSlot::slot_id() const noexcept {
    return impl_->data.slot_id;
}

const std::string& GateTestSlot::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}

const std::vector<GatePublicExample>& GateTestSlot::public_examples() const noexcept {
    return impl_->data.public_examples;
}

const std::vector<GateHiddenCase>& GateTestSlot::hidden_cases() const noexcept {
    return impl_->data.hidden_cases;
}

const std::optional<GatePropertyModule>& GateTestSlot::property_module() const noexcept {
    return impl_->data.property_module;
}

const std::optional<GateCaseGenerator>& GateTestSlot::case_generator() const noexcept {
    return impl_->data.case_generator;
}

const std::vector<GateQualificationMutant>& GateTestSlot::qualification_mutants() const noexcept {
    return impl_->data.qualification_mutants;
}

const std::vector<GateGoldenCandidate>& GateTestSlot::golden_candidates() const noexcept {
    return impl_->data.golden_candidates;
}

std::string GateTestSlot::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool GateTestSlot::operator==(const GateTestSlot& other) const noexcept {
    return impl_->digest == other.impl_->digest;
}

struct ProgramGateVersion::Impl {
    ProgramGateData data;
    std::string     canonical_bytes;
    std::string     id;
};

ProgramGateVersion::ProgramGateVersion(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

ProgramGateVersion ProgramGateVersion::create(ProgramGateData data) {
    validate_gate_data(data);
    auto impl             = std::make_shared<Impl>();
    impl->canonical_bytes = canonical_json_bytes(gate_version_to_json(data));
    impl->id              = sha256_identity(kGateVersionDomain, impl->canonical_bytes);
    impl->data            = std::move(data);
    return ProgramGateVersion(std::move(impl));
}

ProgramGateVersion ProgramGateVersion::parse(std::string_view canonical_bytes) {
    const auto parsed = parse_json_strict(canonical_bytes);
    return create(gate_version_from_json(parsed));
}

const ProgramGateData& ProgramGateVersion::data() const noexcept {
    return impl_->data;
}

const std::string& ProgramGateVersion::id() const noexcept {
    return impl_->id;
}

const std::string& ProgramGateVersion::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}

const std::string& ProgramGateVersion::gate_name() const noexcept {
    return impl_->data.gate_name;
}

const std::string& ProgramGateVersion::test_slot_digest() const noexcept {
    return impl_->data.test_slot_digest;
}

const GateSandboxLimits& ProgramGateVersion::sandbox_limits() const noexcept {
    return impl_->data.sandbox_limits;
}

const GateFeedbackPolicy& ProgramGateVersion::feedback_policy() const noexcept {
    return impl_->data.feedback_policy;
}

std::string ProgramGateVersion::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool ProgramGateVersion::operator==(const ProgramGateVersion& other) const noexcept {
    return impl_->id == other.impl_->id;
}

struct GateVerdictToken::Impl {
    GateVerdictTokenData data;
    std::string          canonical_bytes;
    std::string          id;
};

GateVerdictToken::GateVerdictToken(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::string GateVerdictToken::compute_value_digest(const ProgramSource& candidate) {
    return sha256_identity(kGateValueDomain, candidate.serialize_canonical());
}

std::string GateVerdictToken::compute_value_digest(const json& value) {
    return sha256_identity(kGateValueDomain, canonical_json_bytes(value));
}

GateVerdictToken GateVerdictToken::create(GateVerdictTokenData data) {
    validate_token_data(data);
    auto impl             = std::make_shared<Impl>();
    impl->canonical_bytes = canonical_json_bytes(token_to_json(data));
    impl->id              = sha256_identity(kGateVerdictTokenDomain, impl->canonical_bytes);
    impl->data            = std::move(data);
    return GateVerdictToken(std::move(impl));
}

GateVerdictToken GateVerdictToken::parse(std::string_view canonical_bytes) {
    const auto parsed = parse_json_strict(canonical_bytes);
    return create(token_from_json(parsed));
}

const GateVerdictTokenData& GateVerdictToken::data() const noexcept {
    return impl_->data;
}

const std::string& GateVerdictToken::id() const noexcept {
    return impl_->id;
}

const std::string& GateVerdictToken::owner_scope() const noexcept {
    return impl_->data.owner_scope;
}

const std::string& GateVerdictToken::subject_id() const noexcept {
    return impl_->data.subject_id;
}

const std::string& GateVerdictToken::gate_version_id() const noexcept {
    return impl_->data.gate_version_id;
}

const std::string& GateVerdictToken::test_slot_digest() const noexcept {
    return impl_->data.test_slot_digest;
}

const std::string& GateVerdictToken::checked_value_digest() const noexcept {
    return impl_->data.checked_value_digest;
}

std::uint64_t GateVerdictToken::attempt() const noexcept {
    return impl_->data.attempt;
}

const std::string& GateVerdictToken::generator_seed_hex() const noexcept {
    return impl_->data.generator_seed_hex;
}

bool GateVerdictToken::binds(const ProgramSource& candidate) const {
    return impl_->data.checked_value_digest == compute_value_digest(candidate);
}

bool GateVerdictToken::binds_value(const json& value) const {
    return impl_->data.checked_value_digest == compute_value_digest(value);
}

std::string GateVerdictToken::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool GateVerdictToken::operator==(const GateVerdictToken& other) const noexcept {
    return impl_->id == other.impl_->id;
}

struct GateSlotQualificationReceipt::Impl {
    GateSlotQualificationReceiptData data;
    std::string                      canonical_bytes;
    std::string                      id;
};

GateSlotQualificationReceipt::GateSlotQualificationReceipt(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

GateSlotQualificationReceipt
GateSlotQualificationReceipt::create(GateSlotQualificationReceiptData data) {
    validate_receipt_data(data);
    auto impl             = std::make_shared<Impl>();
    impl->canonical_bytes = canonical_json_bytes(receipt_to_json(data));
    impl->id              = sha256_identity(kGateQualificationReceiptDomain, impl->canonical_bytes);
    impl->data            = std::move(data);
    return GateSlotQualificationReceipt(std::move(impl));
}

GateSlotQualificationReceipt GateSlotQualificationReceipt::parse(std::string_view canonical_bytes) {
    const auto parsed = parse_json_strict(canonical_bytes);
    return create(receipt_from_json(parsed));
}

const GateSlotQualificationReceiptData& GateSlotQualificationReceipt::data() const noexcept {
    return impl_->data;
}

const std::string& GateSlotQualificationReceipt::id() const noexcept {
    return impl_->id;
}

const std::string& GateSlotQualificationReceipt::gate_version_id() const noexcept {
    return impl_->data.gate_version_id;
}

const std::string& GateSlotQualificationReceipt::test_slot_digest() const noexcept {
    return impl_->data.test_slot_digest;
}

const std::string& GateSlotQualificationReceipt::mutants_digest() const noexcept {
    return impl_->data.mutants_digest;
}

std::uint64_t GateSlotQualificationReceipt::total_mutants() const noexcept {
    return impl_->data.total_mutants;
}

std::uint64_t GateSlotQualificationReceipt::killed_mutants() const noexcept {
    return impl_->data.killed_mutants;
}

bool GateSlotQualificationReceipt::golden_passed() const noexcept {
    return impl_->data.golden_passed;
}

const std::string& GateSlotQualificationReceipt::generator_seed_hex() const noexcept {
    return impl_->data.generator_seed_hex;
}

std::string GateSlotQualificationReceipt::serialize_canonical() const {
    return impl_->canonical_bytes;
}

bool GateSlotQualificationReceipt::operator==(
    const GateSlotQualificationReceipt& other) const noexcept {
    return impl_->id == other.impl_->id;
}

GateEvaluation GateEvaluator::evaluate(const ProgramGateVersion& gate,
                                       const GateTestSlot&       slot,
                                       const ProgramSource&      candidate,
                                       std::string_view          subject_id,
                                       std::uint64_t             attempt,
                                       std::string_view          generator_seed_hex) const {
    validate_identifier(subject_id, "subject_id");
    if (slot.owner_scope() != gate.owner_scope()) {
        throw_gate_diagnostic(
            slot.slot_id(),
            "P_GATE_SLOT_OWNER_MISMATCH",
            "Gate test slot owner_scope does not match the Program gate owner_scope",
            json{{"gate_owner_scope", gate.owner_scope()},
                 {"slot_owner_scope", slot.owner_scope()}});
    }
    if (slot.digest() != gate.test_slot_digest()) {
        throw_gate_diagnostic(
            slot.slot_id(),
            "P_GATE_SLOT_DIGEST_MISMATCH",
            "Gate test slot digest does not match the Program gate version test_slot_digest",
            json{{"expected_test_slot_digest", gate.test_slot_digest()},
                 {"actual_test_slot_digest", slot.digest()}});
    }

    const std::string candidate_script = extract_javascript_source_text(candidate);
    if (!candidate.imports().empty() || !candidate.sealed_modules().empty()) {
        throw_gate_diagnostic(candidate.source_id(),
                              "P_GATE_SANDBOX_IMPORT_DENIED",
                              "Program gate sandbox denies JavaScript module imports",
                              json{{"engine", "quickjs"}, {"facility", "import"}});
    }

    const auto& limits = gate.sandbox_limits();
    detail::validate_gate_javascript_module_no_imports(
        candidate.source_id(), candidate_script, limits);

    if (slot.property_module().has_value()) {
        detail::validate_gate_javascript_module_no_imports(
            slot.slot_id(), slot.property_module()->source, limits);
    }
    if (slot.case_generator().has_value()) {
        if (generator_seed_hex.empty()) {
            throw_gate_diagnostic(
                slot.slot_id(),
                "P_GATE_SEED_REQUIRED",
                "GateEvaluator::evaluate requires a non-empty generator_seed_hex when "
                "case_generator is configured");
        }
        validate_utf8(generator_seed_hex);
        validate_token(generator_seed_hex, "generator_seed_hex");
        detail::validate_gate_javascript_module_no_imports(
            slot.slot_id(), slot.case_generator()->source, limits);
    }

    std::uint64_t            total_cases  = 0;
    std::uint64_t            failed_cases = 0;
    std::optional<json>      first_failed_public_example{};
    std::vector<std::string> failure_categories;

    for (const auto& example : slot.public_examples()) {
        ++total_cases;
        const auto run = detail::evaluate_gate_candidate_case(
            candidate.source_id(), candidate_script, example.input, limits);
        if (!run.passed || !run.actual.has_value()) {
            ++failed_cases;
            record_failure_category(
                failure_categories,
                run.failure.value_or(GateFailureCategory::RuntimeException));
            if (!first_failed_public_example.has_value()) {
                first_failed_public_example = json{{"case_id", example.case_id},
                                                   {"input", example.input},
                                                   {"expected", example.expected},
                                                   {"actual", nullptr}};
            }
            continue;
        }
        if (canonical_json_bytes(*run.actual) != canonical_json_bytes(example.expected)) {
            ++failed_cases;
            record_failure_category(failure_categories, GateFailureCategory::AssertionFailed);
            if (!first_failed_public_example.has_value()) {
                first_failed_public_example = json{{"case_id", example.case_id},
                                                   {"input", example.input},
                                                   {"expected", example.expected},
                                                   {"actual", *run.actual}};
            }
        }
    }

    for (const auto& hidden : slot.hidden_cases()) {
        ++total_cases;
        const auto run = detail::evaluate_gate_candidate_case(
            candidate.source_id(), candidate_script, hidden.input, limits);
        if (!run.passed || !run.actual.has_value()) {
            ++failed_cases;
            record_failure_category(
                failure_categories,
                run.failure.value_or(GateFailureCategory::RuntimeException));
            continue;
        }
        if (hidden.expected.has_value()) {
            if (canonical_json_bytes(*run.actual) != canonical_json_bytes(*hidden.expected)) {
                ++failed_cases;
                record_failure_category(failure_categories, GateFailureCategory::AssertionFailed);
            }
        } else {
            const auto prop = detail::evaluate_gate_property_case(
                slot.slot_id(),
                slot.property_module()->source,
                *hidden.property_entrypoint,
                hidden.input,
                *run.actual,
                limits);
            if (!prop.passed) {
                ++failed_cases;
                record_failure_category(
                    failure_categories,
                    prop.failure.value_or(GateFailureCategory::AssertionFailed));
            }
        }
    }

    if (slot.case_generator().has_value()) {
        const auto& generator = *slot.case_generator();
        for (std::uint64_t index = 0; index < generator.generated_case_count; ++index) {
            ++total_cases;
            detail::GateCaseSandboxResult generated;
            try {
                generated = detail::generate_gate_case_input(
                    slot.slot_id(), generator.source, generator_seed_hex, index, limits);
            } catch (const ProgramDiagnosticError& error) {
                if (error.diagnostic().code == "P_JS_UNAVAILABLE") {
                    throw;
                }
                ++failed_cases;
                record_failure_category(failure_categories, GateFailureCategory::RuntimeException);
                continue;
            } catch (const std::exception&) {
                ++failed_cases;
                record_failure_category(failure_categories, GateFailureCategory::RuntimeException);
                continue;
            }
            if (!generated.passed || !generated.actual.has_value()) {
                ++failed_cases;
                const auto category =
                    (generated.failure == GateFailureCategory::Timeout ||
                     generated.failure == GateFailureCategory::MemoryLimit)
                        ? *generated.failure
                        : GateFailureCategory::RuntimeException;
                record_failure_category(failure_categories, category);
                continue;
            }
            const json& generated_input = *generated.actual;
            const auto  run             = detail::evaluate_gate_candidate_case(
                candidate.source_id(), candidate_script, generated_input, limits);
            if (!run.passed || !run.actual.has_value()) {
                ++failed_cases;
                record_failure_category(
                    failure_categories,
                    run.failure.value_or(GateFailureCategory::RuntimeException));
                continue;
            }
            const auto prop = detail::evaluate_gate_property_case(
                slot.slot_id(),
                slot.property_module()->source,
                generator.property_entrypoint,
                generated_input,
                *run.actual,
                limits);
            if (!prop.passed) {
                ++failed_cases;
                record_failure_category(
                    failure_categories,
                    prop.failure.value_or(GateFailureCategory::AssertionFailed));
            }
        }
    }

    const bool passed = total_cases > 0 && failed_cases == 0;

    GateEvaluation evaluation;
    evaluation.verdict.passed          = passed;
    evaluation.verdict.total_cases     = total_cases;
    evaluation.verdict.failed_cases    = failed_cases;
    evaluation.verdict.public_feedback = build_public_feedback(gate.feedback_policy().mode,
                                                               passed,
                                                               total_cases,
                                                               failed_cases,
                                                               first_failed_public_example,
                                                               failure_categories);
    evaluation.verdict.failure_categories = std::move(failure_categories);

    if (passed) {
        evaluation.token = GateVerdictToken::create(GateVerdictTokenData{
            gate.owner_scope(),
            std::string(subject_id),
            gate.id(),
            slot.digest(),
            GateVerdictToken::compute_value_digest(candidate),
            attempt,
            slot.case_generator().has_value() ? std::string(generator_seed_hex) : std::string{}});
    }
    return evaluation;
}

GateSlotQualificationReceipt GateEvaluator::qualify(const ProgramGateVersion& gate,
                                                    const GateTestSlot&       slot) const {
    if (slot.owner_scope() != gate.owner_scope()) {
        throw_gate_diagnostic(
            slot.slot_id(),
            "P_GATE_SLOT_OWNER_MISMATCH",
            "Gate test slot owner_scope does not match the Program gate owner_scope",
            json{{"gate_owner_scope", gate.owner_scope()},
                 {"slot_owner_scope", slot.owner_scope()}});
    }
    if (slot.digest() != gate.test_slot_digest()) {
        throw_gate_diagnostic(
            slot.slot_id(),
            "P_GATE_SLOT_DIGEST_MISMATCH",
            "Gate test slot digest does not match the Program gate version test_slot_digest",
            json{{"expected_test_slot_digest", gate.test_slot_digest()},
                 {"actual_test_slot_digest", slot.digest()}});
    }
    if (slot.qualification_mutants().empty()) {
        throw_gate_diagnostic(
            slot.slot_id(),
            "P_GATE_SLOT_NO_MUTANTS",
            "Gate test slot must declare at least one qualification mutant");
    }

    const auto&       limits   = gate.sandbox_limits();
    const std::string seed_hex = default_qualification_seed(slot);

    if (slot.property_module().has_value()) {
        detail::validate_gate_javascript_module_no_imports(
            slot.slot_id(), slot.property_module()->source, limits);
    }
    if (slot.case_generator().has_value()) {
        const auto& generator = *slot.case_generator();
        detail::validate_gate_javascript_module_no_imports(
            slot.slot_id(), generator.source, limits);
        for (std::uint64_t index = 0; index < generator.generated_case_count; ++index) {
            detail::GateCaseSandboxResult generated;
            try {
                generated = detail::generate_gate_case_input(
                    slot.slot_id(), generator.source, seed_hex, index, limits);
            } catch (const ProgramDiagnosticError& error) {
                if (error.diagnostic().code == "P_JS_UNAVAILABLE") {
                    throw;
                }
            } catch (const std::exception&) {
            }
            if (!generated.passed || !generated.actual.has_value()) {
                throw_gate_diagnostic(
                    slot.slot_id(),
                    "P_GATE_SLOT_GENERATOR_FAILED",
                    "Gate test slot case_generator failed to generate case index " +
                        std::to_string(index),
                    json{{"case_index", index}});
            }
        }
    }

    // A slot that accepts no known-correct logic could reject every mutant by rejecting everything.
    if (slot.golden_candidates().empty()) {
        throw_gate_diagnostic(slot.slot_id(), "P_GATE_SLOT_NO_GOLDEN",
                              "Gate test slot has no golden candidate to show that it accepts correct logic",
                              json::object());
    }
    std::uint64_t attempt = 0;
    for (const auto& golden : slot.golden_candidates()) {
        ++attempt;
        const auto source = ProgramSource::from_javascript(golden.candidate_id, golden.source);
        GateEvaluation result;
        try {
            result = evaluate(gate, slot, source, golden.candidate_id, attempt, seed_hex);
        } catch (const ProgramDiagnosticError& error) {
            if (error.diagnostic().code == "P_JS_UNAVAILABLE") {
                throw;
            }
            throw_gate_diagnostic(
                slot.slot_id(),
                "P_GATE_SLOT_GOLDEN_FAILED",
                "Golden candidate '" + golden.candidate_id + "' failed gate evaluation",
                json{{"candidate_id", golden.candidate_id}});
        }
        if (!result.verdict.passed) {
            throw_gate_diagnostic(
                slot.slot_id(),
                "P_GATE_SLOT_GOLDEN_FAILED",
                "Golden candidate '" + golden.candidate_id + "' did not pass every slot case",
                json{{"candidate_id", golden.candidate_id},
                     {"failed_cases", result.verdict.failed_cases},
                     {"total_cases", result.verdict.total_cases}});
        }
    }

    std::uint64_t killed_mutants = 0;
    for (const auto& mutant : slot.qualification_mutants()) {
        ++attempt;
        const auto source = ProgramSource::from_javascript(mutant.mutant_id, mutant.source);
        bool       killed = false;
        try {
            const auto result =
                evaluate(gate, slot, source, mutant.mutant_id, attempt, seed_hex);
            killed = !result.verdict.passed && result.verdict.failed_cases > 0;
        } catch (const ProgramDiagnosticError& error) {
            if (error.diagnostic().code == "P_JS_UNAVAILABLE") {
                throw;
            }
            killed = true;
        }
        if (!killed) {
            throw_gate_diagnostic(
                slot.slot_id(),
                "P_GATE_SLOT_MUTANT_SURVIVED",
                "Qualification mutant '" + mutant.mutant_id + "' passed every slot case",
                json{{"mutant_id", mutant.mutant_id}});
        }
        ++killed_mutants;
    }

    return GateSlotQualificationReceipt::create(GateSlotQualificationReceiptData{
        gate.id(),
        slot.digest(),
        compute_mutants_digest(slot.qualification_mutants()),
        static_cast<std::uint64_t>(slot.qualification_mutants().size()),
        killed_mutants,
        true,
        seed_hex});
}

}  // namespace neograph::program
