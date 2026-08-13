/**
 * @file program/diagnostic.h
 * @brief Stable, transport-neutral Program diagnostics and source coordinates.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::program {

enum class CompilePhase {
    Source,
    Schema,
    Normalize,
    Resolve,
    TypeCheck,
    CoreParse,
    CoreValidate,
    Seal,
};

enum class DiagnosticSeverity { Error, Warning, Note };

struct SourceSpan {
    std::uint64_t byte_begin   = 0;
    std::uint64_t byte_end     = 0;
    std::uint32_t line_begin   = 1;
    std::uint32_t column_begin = 1;
    std::uint32_t line_end     = 1;
    std::uint32_t column_end   = 1;

    bool operator==(const SourceSpan&) const = default;
};

struct SourceCoordinate {
    std::string               source_id;
    std::string               json_pointer;
    std::optional<SourceSpan> span;

    bool operator==(const SourceCoordinate&) const = default;
};

struct Diagnostic {
    CompilePhase                  phase = CompilePhase::Source;
    std::string                   code;
    DiagnosticSeverity            severity = DiagnosticSeverity::Error;
    SourceCoordinate              primary;
    std::string                   message;
    json                          witness;
    std::vector<SourceCoordinate> related;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(CompilePhase phase) noexcept;
NEOGRAPH_PROGRAM_API std::string_view   to_string(DiagnosticSeverity severity) noexcept;
NEOGRAPH_PROGRAM_API CompilePhase       compile_phase_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API DiagnosticSeverity diagnostic_severity_from_string(std::string_view value);

NEOGRAPH_PROGRAM_API void to_json(json& value, const SourceSpan& span);
NEOGRAPH_PROGRAM_API void from_json(const json& value, SourceSpan& span);
NEOGRAPH_PROGRAM_API void to_json(json& value, const SourceCoordinate& coordinate);
NEOGRAPH_PROGRAM_API void from_json(const json& value, SourceCoordinate& coordinate);
NEOGRAPH_PROGRAM_API void to_json(json& value, const Diagnostic& diagnostic);
NEOGRAPH_PROGRAM_API void from_json(const json& value, Diagnostic& diagnostic);

class NEOGRAPH_PROGRAM_API ProgramDiagnosticError : public std::runtime_error {
public:
    explicit ProgramDiagnosticError(Diagnostic diagnostic);
    const Diagnostic& diagnostic() const noexcept;

private:
    Diagnostic diagnostic_;
};

}  // namespace neograph::program
