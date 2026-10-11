#include <neograph/program/diagnostic.h>

#include "canonical_json.h"

#include <algorithm>
#include <iterator>
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
    if (diagnostic.help) {
        json help;
        to_json(help, *diagnostic.help);
        value["help"] = std::move(help);
    }
}

void from_json(const json& value, Diagnostic& diagnostic) {
    require_object(value, "Program diagnostic");
    detail::reject_unknown_fields(
        value, "Program diagnostic",
        {"phase", "code", "severity", "primary", "message", "witness", "related", "help"});
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
    diagnostic.witness = detail::owned_json_copy(value["witness"]);
    if (!value.contains("related") || !value["related"].is_array()) {
        throw std::invalid_argument("Program diagnostic related field must be an array");
    }
    diagnostic.related.clear();
    for (const auto& item : value["related"]) {
        SourceCoordinate coordinate;
        from_json(item, coordinate);
        diagnostic.related.push_back(std::move(coordinate));
    }
    diagnostic.help.reset();
    if (value.contains("help")) {
        DiagnosticHelp help;
        from_json(value["help"], help);
        diagnostic.help = std::move(help);
    }
    if (diagnostic.code.empty() || diagnostic.message.empty()) {
        throw std::invalid_argument("Program diagnostic requires code and message");
    }
}

std::string_view to_string(SuggestionApplicability applicability) noexcept {
    switch (applicability) {
        case SuggestionApplicability::Machine:
            return "machine";
        case SuggestionApplicability::Maybe:
            return "maybe";
        case SuggestionApplicability::Placeholder:
            return "placeholder";
    }
    return "unknown";
}

SuggestionApplicability suggestion_applicability_from_string(std::string_view value) {
    if (value == "machine") return SuggestionApplicability::Machine;
    if (value == "maybe") return SuggestionApplicability::Maybe;
    if (value == "placeholder") return SuggestionApplicability::Placeholder;
    throw std::invalid_argument("Unknown Program suggestion applicability: " + std::string(value));
}

namespace {

// Codes with their own entry in docs/diagnostics.md, sorted for binary search.
// tests/test_program_diagnostics_help.cpp checks that each one has an anchor.
constexpr std::string_view kDocumentedCodes[] = {
    "P_BUDGET_INVALID",
    "P_BUDGET_STATIC",
    "P_BUDGET_V1",
    "P_COMPILER_INTERNAL",
    "P_CONTRACT_SCHEMA",
    "P_CORE_BARRIER_STALLED",
    "P_CORE_E10",
    "P_CORE_E11",
    "P_CORE_E12",
    "P_CORE_E13",
    "P_CORE_E14",
    "P_CORE_E15",
    "P_CORE_E16",
    "P_CORE_E17",
    "P_CORE_E3",
    "P_CORE_E4",
    "P_CORE_E5",
    "P_CORE_E6",
    "P_CORE_E7",
    "P_CORE_E8",
    "P_CORE_E9",
    "P_CORE_GC_BARRIER",
    "P_CORE_GC_EDGE",
    "P_CORE_GC_FIELD_REQUIRED",
    "P_CORE_GC_FIELD_TYPE",
    "P_CORE_GC_FIELD_VALUE",
    "P_CORE_GC_METADATA_LIMIT",
    "P_CORE_GC_METADATA_TEXT",
    "P_CORE_GC_NODE_CONFIG",
    "P_CORE_GC_NO_ENTRY",
    "P_CORE_GC_REGISTRY_MISS",
    "P_CORE_GC_RESERVED_NODE",
    "P_CORE_GC_SCHEMA_VERSION",
    "P_CORE_GC_UNKNOWN_FIELD",
    "P_CORE_ROUNDTRIP",
    "P_DIAGNOSTICS_TRUNCATED",
    "P_EXPAND_BOUND",
    "P_EXPAND_FAILURE",
    "P_EXPAND_SOURCE",
    "P_IMPORT_CONFLICT",
    "P_IMPORT_DYNAMIC",
    "P_IMPORT_UNRESOLVED",
    "P_JS_AMBIENT_DENIED",
    "P_JS_CANCELLED",
    "P_JS_CONTROL_ASYNC",
    "P_JS_CONTROL_CHARACTER",
    "P_JS_CONTROL_COMMAND",
    "P_JS_CONTROL_FINISHED",
    "P_JS_CONTROL_MAIN",
    "P_JS_CONTROL_VALUE",
    "P_JS_DEFINE_ASYNC",
    "P_JS_DEFINE_MISSING",
    "P_JS_DEFINE_VALUE",
    "P_JS_EVALUATION",
    "P_JS_GRAPH_ARGUMENT",
    "P_JS_GRAPH_ENTRY",
    "P_JS_GRAPH_LIMIT",
    "P_JS_GRAPH_OWNER",
    "P_JS_GRAPH_SEALED",
    "P_JS_GRAPH_VALUE",
    "P_JS_IMPORT_DYNAMIC",
    "P_JS_IMPORT_INVALID",
    "P_JS_IMPORT_UNAVAILABLE",
    "P_JS_RESOURCE_LIMIT",
    "P_JS_RUNTIME",
    "P_JS_RUNTIME_IDENTITY",
    "P_JS_TIMEOUT",
    "P_JS_UNAVAILABLE",
    "P_MIGRATION_PROGRAM_JSON",
    "P_PARALLEL_MAP_BINDING",
    "P_PARALLEL_MAP_BOUND",
    "P_PARALLEL_MAP_FAILURE",
    "P_PARALLEL_MAP_SOURCE",
    "P_PLAN_BOUND",
    "P_PLAN_CANCEL_SCOPE",
    "P_PLAN_CONDITION",
    "P_PLAN_CORE",
    "P_PLAN_OPERATION",
    "P_PLAN_QUORUM",
    "P_PLAN_RACE_ARITY",
    "P_PLAN_REFERENCE",
    "P_PLAN_SOURCE_POINTER",
    "P_PLAN_SPAWN_SHAPE",
    "P_PLAN_TYPED",
    "P_REGISTRY_DEPENDENCY_MISSING",
    "P_REGISTRY_IDENTITY_MISMATCH",
    "P_REGISTRY_MISSING",
    "P_REGISTRY_REQUIREMENT_KIND",
    "P_REGISTRY_REQUIREMENT_RESOLVER",
    "P_ROOT_NAME",
    "P_ROOT_OPERATION",
    "P_SCHEMA_OPERATION",
    "P_SCHEMA_REQUIRED",
    "P_SCHEMA_TYPE",
    "P_SCHEMA_UNKNOWN_FIELD",
    "P_SCHEMA_VERSION",
};

constexpr std::string_view kDocPath = "docs/diagnostics.md#";

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.substr(0, prefix.size()) == prefix;
}

std::string lowercase(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}

void validate_edit(const SuggestionEdit& edit) {
    detail::validate_json_pointer(edit.path);
    if (edit.op == "move") {
        detail::validate_json_pointer(edit.from);
        if (!edit.value.is_null())
            throw std::invalid_argument("Program suggestion move edits carry no value");
        return;
    }
    if (!edit.from.empty())
        throw std::invalid_argument("Program suggestion edit 'from' is only valid for move");
    if (edit.op == "remove") {
        if (!edit.value.is_null())
            throw std::invalid_argument("Program suggestion remove edits carry no value");
        return;
    }
    if (edit.op != "add" && edit.op != "replace")
        throw std::invalid_argument("Program suggestion edit op must be add, replace, remove or move");
}

json encode_edit(const SuggestionEdit& edit) {
    validate_edit(edit);
    json value    = json::object();
    value["op"]   = edit.op;
    value["path"] = edit.path;
    if (edit.op == "move") value["from"] = edit.from;
    if (edit.op == "add" || edit.op == "replace") value["value"] = edit.value;
    return value;
}

SuggestionEdit decode_edit(const json& value) {
    require_object(value, "Program suggestion edit");
    detail::reject_unknown_fields(value, "Program suggestion edit", {"op", "path", "from", "value"});
    SuggestionEdit edit;
    edit.op   = require_string(value, "op");
    edit.path = require_string(value, "path");
    if (value.contains("from")) edit.from = require_string(value, "from");
    if (edit.op == "move" && !value.contains("from"))
        throw std::invalid_argument("Program suggestion move edits require 'from'");
    if (value.contains("value")) {
        if (edit.op != "add" && edit.op != "replace")
            throw std::invalid_argument("Program suggestion edit carries a value it cannot apply");
        edit.value = detail::owned_json_copy(value["value"]);
    } else if (edit.op == "add" || edit.op == "replace") {
        throw std::invalid_argument("Program suggestion add and replace edits require 'value'");
    }
    validate_edit(edit);
    return edit;
}

}  // namespace

std::string diagnostic_doc_link(std::string_view code) {
    if (std::binary_search(std::begin(kDocumentedCodes), std::end(kDocumentedCodes), code))
        return std::string(kDocPath) + lowercase(code);
    std::string_view family = "runtime-and-admission";
    if (starts_with(code, "P_CORE_GC_"))
        family = "core-parser";
    else if (starts_with(code, "P_CORE_E") && code.size() > 8 && code[8] >= '0' && code[8] <= '9')
        family = "core-validator";
    else if (starts_with(code, "P_JS_") || starts_with(code, "P_IMPORT_"))
        family = "javascript-frontend";
    else if (starts_with(code, "P_REGISTRY_"))
        family = "registry";
    return std::string(kDocPath) + std::string(family);
}

std::size_t diagnostic_name_distance(std::string_view left,
                                     std::string_view right,
                                     std::size_t      limit) {
    const auto difference = left.size() > right.size() ? left.size() - right.size()
                                                        : right.size() - left.size();
    if (difference > limit) return limit + 1;
    // Optimal string alignment: insertions, deletions, substitutions and
    // transpositions of adjacent bytes each cost one.
    std::vector<std::size_t> before(right.size() + 1), previous(right.size() + 1),
        current(right.size() + 1);
    for (std::size_t column = 0; column <= right.size(); ++column) previous[column] = column;
    for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0]       = row;
        std::size_t best = current[0];
        for (std::size_t column = 1; column <= right.size(); ++column) {
            const std::size_t cost = left[row - 1] == right[column - 1] ? 0 : 1;
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
                                        previous[column - 1] + cost});
            if (row > 1 && column > 1 && left[row - 1] == right[column - 2] &&
                left[row - 2] == right[column - 1])
                current[column] = std::min(current[column], before[column - 2] + 1);
            best = std::min(best, current[column]);
        }
        if (best > limit) return limit + 1;
        before.swap(previous);
        previous.swap(current);
    }
    return std::min(previous[right.size()], limit + 1);
}

std::vector<std::string> diagnostic_name_candidates(std::string_view                unknown,
                                                    const std::vector<std::string>& declared) {
    std::vector<std::pair<std::size_t, std::string>> ranked;
    for (const auto& name : declared) {
        if (name == unknown) continue;
        const auto distance = diagnostic_name_distance(unknown, name, 2);
        if (distance <= 2) ranked.emplace_back(distance, name);
    }
    std::sort(ranked.begin(), ranked.end());
    ranked.erase(std::unique(ranked.begin(), ranked.end()), ranked.end());
    std::vector<std::string> result;
    for (auto& [distance, name] : ranked) {
        (void)distance;
        if (result.empty() || result.back() != name) result.push_back(std::move(name));
    }
    return result;
}

Diagnostic sealed_diagnostic(Diagnostic diagnostic) {
    diagnostic.help.reset();
    return diagnostic;
}

void to_json(json& value, const DiagnosticHelp& help) {
    value        = json::object();
    value["doc"] = help.doc;
    json related = json::array();
    for (const auto& item : help.related) {
        json encoded;
        to_json(encoded, item.coordinate);
        encoded["label"] = item.label;
        related.push_back(std::move(encoded));
    }
    value["related"]  = std::move(related);
    json suggestions = json::array();
    for (const auto& suggestion : help.suggestions) {
        json edits = json::array();
        for (const auto& edit : suggestion.edits) edits.push_back(encode_edit(edit));
        suggestions.push_back(
            json{{"message", suggestion.message},
                 {"edits", std::move(edits)},
                 {"applicability", std::string(to_string(suggestion.applicability))}});
    }
    value["suggestions"] = std::move(suggestions);
    json candidates      = json::array();
    for (const auto& candidate : help.candidates) candidates.push_back(candidate);
    value["candidates"] = std::move(candidates);
}

void from_json(const json& value, DiagnosticHelp& help) {
    require_object(value, "Program diagnostic help");
    detail::reject_unknown_fields(value, "Program diagnostic help",
                                  {"doc", "related", "suggestions", "candidates"});
    help          = DiagnosticHelp{};
    help.doc      = require_string(value, "doc");
    const auto array = [&](std::string_view key) -> json {
        const std::string owned(key);
        if (!value.contains(owned) || !value[owned].is_array())
            throw std::invalid_argument("Program diagnostic help field '" + owned +
                                        "' must be an array");
        return value[owned];
    };
    const json related_items = array("related");
    for (const auto& item : related_items) {
        require_object(item, "Program diagnostic related location");
        json coordinate = json::object();
        for (const auto& [key, field] : item.items())
            if (key != "label") coordinate[key] = field;
        LabelledCoordinate related;
        related.label = require_string(item, "label");
        from_json(coordinate, related.coordinate);
        help.related.push_back(std::move(related));
    }
    const json suggestion_items = array("suggestions");
    for (const auto& item : suggestion_items) {
        require_object(item, "Program diagnostic suggestion");
        detail::reject_unknown_fields(item, "Program diagnostic suggestion",
                                      {"message", "edits", "applicability"});
        DiagnosticSuggestion suggestion;
        suggestion.message = require_string(item, "message");
        suggestion.applicability =
            suggestion_applicability_from_string(require_string(item, "applicability"));
        if (!item.contains("edits") || !item["edits"].is_array())
            throw std::invalid_argument("Program diagnostic suggestion edits must be an array");
        for (const auto& edit : item["edits"]) suggestion.edits.push_back(decode_edit(edit));
        help.suggestions.push_back(std::move(suggestion));
    }
    const json candidate_items = array("candidates");
    for (const auto& item : candidate_items) {
        if (!item.is_string())
            throw std::invalid_argument("Program diagnostic candidates must be strings");
        help.candidates.push_back(item.get<std::string>());
    }
}

ProgramDiagnosticError::ProgramDiagnosticError(Diagnostic diagnostic)
    : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {
    diagnostic_.witness = detail::owned_json_copy(diagnostic_.witness);
}

const Diagnostic& ProgramDiagnosticError::diagnostic() const noexcept {
    return diagnostic_;
}

}  // namespace neograph::program

