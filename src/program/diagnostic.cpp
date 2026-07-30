#include <neograph/program/diagnostic.h>

#include "canonical_json.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace neograph::program {
namespace {

void require_object(const json& value, std::string_view name) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(name) + " must be an object");
    }
}

std::string require_string(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_string()) {
        throw std::invalid_argument("Program diagnostic field '" + owned_key +
                                    "' must be a string");
    }
    return value[owned_key].get<std::string>();
}

std::uint64_t require_uint64(const json& value, std::string_view key) {
    const std::string owned_key(key);
    if (!value.contains(owned_key) || !value[owned_key].is_number_unsigned()) {
        throw std::invalid_argument("Program diagnostic field '" + owned_key +
                                    "' must be unsigned");
    }
    return value[owned_key].get<unsigned long long>();
}

std::uint32_t require_uint32(const json& value, std::string_view key) {
    const auto number = require_uint64(value, key);
    if (number > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Program diagnostic integer exceeds uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

void validate_span(const SourceSpan& span) {
    const bool line_column_reversed =
        span.line_end < span.line_begin ||
        (span.line_end == span.line_begin && span.column_end < span.column_begin);
    if (span.byte_end < span.byte_begin || line_column_reversed || span.line_begin == 0 ||
        span.column_begin == 0 || span.line_end == 0 || span.column_end == 0) {
        throw std::invalid_argument("Program source span is invalid");
    }
}

void validate_coordinate(const SourceCoordinate& coordinate) {
    if (coordinate.source_id.empty()) {
        throw std::invalid_argument("Program source coordinate requires source_id");
    }
    detail::validate_utf8(coordinate.source_id);
    detail::validate_json_pointer(coordinate.json_pointer);
    if (coordinate.span) validate_span(*coordinate.span);
}

bool valid_phase(CompilePhase phase) noexcept {
    switch (phase) {
        case CompilePhase::Source:
        case CompilePhase::Schema:
        case CompilePhase::Normalize:
        case CompilePhase::Resolve:
        case CompilePhase::TypeCheck:
        case CompilePhase::CoreParse:
        case CompilePhase::CoreValidate:
        case CompilePhase::Seal:
            return true;
    }
    return false;
}

bool valid_severity(DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case DiagnosticSeverity::Error:
        case DiagnosticSeverity::Warning:
        case DiagnosticSeverity::Note:
            return true;
    }
    return false;
}

}  // namespace

std::string_view to_string(CompilePhase phase) noexcept {
    switch (phase) {
        case CompilePhase::Source:
            return "source";
        case CompilePhase::Schema:
            return "schema";
        case CompilePhase::Normalize:
            return "normalize";
        case CompilePhase::Resolve:
            return "resolve";
        case CompilePhase::TypeCheck:
            return "type_check";
        case CompilePhase::CoreParse:
            return "core_parse";
        case CompilePhase::CoreValidate:
            return "core_validate";
        case CompilePhase::Seal:
            return "seal";
    }
    return "unknown";
}

std::string_view to_string(DiagnosticSeverity severity) noexcept {
    switch (severity) {
        case DiagnosticSeverity::Error:
            return "error";
        case DiagnosticSeverity::Warning:
            return "warning";
        case DiagnosticSeverity::Note:
            return "note";
    }
    return "unknown";
}

CompilePhase compile_phase_from_string(std::string_view value) {
    if (value == "source") return CompilePhase::Source;
    if (value == "schema") return CompilePhase::Schema;
    if (value == "normalize") return CompilePhase::Normalize;
    if (value == "resolve") return CompilePhase::Resolve;
    if (value == "type_check") return CompilePhase::TypeCheck;
    if (value == "core_parse") return CompilePhase::CoreParse;
    if (value == "core_validate") return CompilePhase::CoreValidate;
    if (value == "seal") return CompilePhase::Seal;
    throw std::invalid_argument("Unknown Program compile phase: " + std::string(value));
}

DiagnosticSeverity diagnostic_severity_from_string(std::string_view value) {
    if (value == "error") return DiagnosticSeverity::Error;
    if (value == "warning") return DiagnosticSeverity::Warning;
    if (value == "note") return DiagnosticSeverity::Note;
    throw std::invalid_argument("Unknown Program diagnostic severity: " + std::string(value));
}

void to_json(json& value, const SourceSpan& span) {
    validate_span(span);
    value                 = json::object();
    value["byte_begin"]   = span.byte_begin;
    value["byte_end"]     = span.byte_end;
    value["line_begin"]   = span.line_begin;
    value["column_begin"] = span.column_begin;
    value["line_end"]     = span.line_end;
    value["column_end"]   = span.column_end;
}

void from_json(const json& value, SourceSpan& span) {
    require_object(value, "Program source span");
    detail::reject_unknown_fields(
        value, "Program source span",
        {"byte_begin", "byte_end", "line_begin", "column_begin", "line_end", "column_end"});
    span.byte_begin   = require_uint64(value, "byte_begin");
    span.byte_end     = require_uint64(value, "byte_end");
    span.line_begin   = require_uint32(value, "line_begin");
    span.column_begin = require_uint32(value, "column_begin");
    span.line_end     = require_uint32(value, "line_end");
    span.column_end   = require_uint32(value, "column_end");
    validate_span(span);
}

void to_json(json& value, const SourceCoordinate& coordinate) {
    validate_coordinate(coordinate);
    value                 = json::object();
    value["source_id"]    = coordinate.source_id;
    value["json_pointer"] = coordinate.json_pointer;
    if (coordinate.span) {
        json encoded_span;
        to_json(encoded_span, *coordinate.span);
        value["span"] = std::move(encoded_span);
    } else {
        value["span"] = nullptr;
    }
}

void from_json(const json& value, SourceCoordinate& coordinate) {
    require_object(value, "Program source coordinate");
    detail::reject_unknown_fields(value, "Program source coordinate",
                                  {"source_id", "json_pointer", "span"});
    coordinate.source_id    = require_string(value, "source_id");
    coordinate.json_pointer = require_string(value, "json_pointer");
    coordinate.span.reset();
    if (!value.contains("span")) {
        throw std::invalid_argument("Program source coordinate requires explicit span field");
    }
    if (!value["span"].is_null()) {
        SourceSpan span;
        from_json(value["span"], span);
        coordinate.span = span;
    }
    validate_coordinate(coordinate);
}

void to_json(json& value, const Diagnostic& diagnostic) {
    if (diagnostic.code.empty() || diagnostic.message.empty()) {
        throw std::invalid_argument("Program diagnostic requires code and message");
    }
    if (!valid_phase(diagnostic.phase) || !valid_severity(diagnostic.severity)) {
        throw std::invalid_argument("Program diagnostic contains an unknown enum value");
    }
    value             = json::object();
    value["phase"]    = std::string(to_string(diagnostic.phase));
    value["code"]     = diagnostic.code;
    value["severity"] = std::string(to_string(diagnostic.severity));
    json primary;
    to_json(primary, diagnostic.primary);
    value["primary"] = std::move(primary);
    value["message"] = diagnostic.message;
    value["witness"] = diagnostic.witness;
    json related     = json::array();
    for (const auto& coordinate : diagnostic.related) {
        json encoded;
        to_json(encoded, coordinate);
        related.push_back(std::move(encoded));
    }
    value["related"] = std::move(related);
}

void from_json(const json& value, Diagnostic& diagnostic) {
    require_object(value, "Program diagnostic");
    detail::reject_unknown_fields(
        value, "Program diagnostic",
        {"phase", "code", "severity", "primary", "message", "witness", "related"});
    diagnostic.phase    = compile_phase_from_string(require_string(value, "phase"));
    diagnostic.code     = require_string(value, "code");
    diagnostic.severity = diagnostic_severity_from_string(require_string(value, "severity"));
    if (!value.contains("primary")) {
        throw std::invalid_argument("Program diagnostic requires primary coordinate");
    }
    from_json(value["primary"], diagnostic.primary);
    diagnostic.message = require_string(value, "message");
    if (!value.contains("witness")) {
        throw std::invalid_argument("Program diagnostic requires witness");
    }
    diagnostic.witness = value["witness"];
    if (!value.contains("related") || !value["related"].is_array()) {
        throw std::invalid_argument("Program diagnostic related field must be an array");
    }
    diagnostic.related.clear();
    for (const auto& item : value["related"]) {
        SourceCoordinate coordinate;
        from_json(item, coordinate);
        diagnostic.related.push_back(std::move(coordinate));
    }
    if (diagnostic.code.empty() || diagnostic.message.empty()) {
        throw std::invalid_argument("Program diagnostic requires code and message");
    }
}

ProgramDiagnosticError::ProgramDiagnosticError(Diagnostic diagnostic)
    : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {}

const Diagnostic& ProgramDiagnosticError::diagnostic() const noexcept {
    return diagnostic_;
}

}  // namespace neograph::program
