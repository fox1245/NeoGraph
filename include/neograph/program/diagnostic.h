/**
 * @file program/diagnostic.h
 * @brief Stable, transport-neutral Program diagnostics and source coordinates.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/configuration_failure.h>
#include <neograph/json.h>

#include <cstddef>
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

/// How safely a tool may apply a suggestion without review.
enum class SuggestionApplicability {
    Machine,      ///< The edits are the fix; apply them as given.
    Maybe,        ///< A likely fix; review it first. The default.
    Placeholder,  ///< The edits contain a value the author must fill in.
};

/**
 * One edit of a suggestion, as an RFC 6902 JSON Patch operation on the
 * Program document the compiler parsed. For a JavaScript source that is the
 * document define() generated; the diagnostic's call site locates it.
 * `op` is "add", "replace", "remove" or "move"; `from` is set only for "move"
 * and `value` is null for "remove" and "move".
 */
struct SuggestionEdit {
    std::string op;
    std::string path;
    std::string from;
    json        value;
};

struct DiagnosticSuggestion {
    std::string                 message;
    std::vector<SuggestionEdit> edits;
    SuggestionApplicability     applicability = SuggestionApplicability::Maybe;
};

/// A secondary location with a one-line label ("also referenced here").
struct LabelledCoordinate {
    std::string      label;
    SourceCoordinate coordinate;
};

/**
 * Self-repair help for one diagnostic. It is part of the diagnostics a
 * compilation returns and of Program exceptions; it is never sealed into a
 * bundle, so it can improve without changing any bundle id.
 */
struct DiagnosticHelp {
    /// Repository-relative documentation link, e.g. "docs/diagnostics.md#p_core_e15".
    std::string                       doc;
    std::vector<LabelledCoordinate>   related;
    std::vector<DiagnosticSuggestion> suggestions;
    /// Declared names close to an unknown one (Damerau distance <= 2), nearest first.
    std::vector<std::string> candidates;
};

struct Diagnostic {
    CompilePhase                  phase = CompilePhase::Source;
    std::string                   code;
    DiagnosticSeverity            severity = DiagnosticSeverity::Error;
    SourceCoordinate              primary;
    std::string                   message;
    json                          witness;
    std::vector<SourceCoordinate> related;
    /// Present on returned diagnostics; always absent from sealed ones.
    std::optional<DiagnosticHelp> help;
};

/// Code of the note that replaces returned diagnostics past the cap.
inline constexpr std::string_view DIAGNOSTICS_TRUNCATED_CODE = "P_DIAGNOSTICS_TRUNCATED";
/// At most this many diagnostics are returned, plus the truncation note.
inline constexpr std::size_t MAX_RETURNED_DIAGNOSTICS = 20;

NEOGRAPH_PROGRAM_API std::string_view to_string(CompilePhase phase) noexcept;
NEOGRAPH_PROGRAM_API std::string_view   to_string(DiagnosticSeverity severity) noexcept;
NEOGRAPH_PROGRAM_API std::string_view   to_string(SuggestionApplicability applicability) noexcept;
NEOGRAPH_PROGRAM_API CompilePhase       compile_phase_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API DiagnosticSeverity diagnostic_severity_from_string(std::string_view value);
NEOGRAPH_PROGRAM_API SuggestionApplicability
suggestion_applicability_from_string(std::string_view value);

/**
 * The documentation link for a diagnostic code: its own entry in
 * docs/diagnostics.md, or the section of its family when the code has none.
 */
NEOGRAPH_PROGRAM_API std::string diagnostic_doc_link(std::string_view code);

/**
 * Damerau-Levenshtein (optimal string alignment) distance, counted in bytes.
 * Returns `limit + 1` as soon as the distance is known to exceed `limit`.
 */
NEOGRAPH_PROGRAM_API std::size_t diagnostic_name_distance(std::string_view left,
                                                          std::string_view right,
                                                          std::size_t      limit = 2);

/// Names within distance 2 of `unknown`, nearest first, then by byte order.
NEOGRAPH_PROGRAM_API std::vector<std::string> diagnostic_name_candidates(
    std::string_view unknown, const std::vector<std::string>& declared);

/// The copy that is sealed into a bundle: the same diagnostic without help.
NEOGRAPH_PROGRAM_API Diagnostic sealed_diagnostic(Diagnostic diagnostic);

NEOGRAPH_PROGRAM_API void to_json(json& value, const SourceSpan& span);
NEOGRAPH_PROGRAM_API void from_json(const json& value, SourceSpan& span);
NEOGRAPH_PROGRAM_API void to_json(json& value, const SourceCoordinate& coordinate);
NEOGRAPH_PROGRAM_API void from_json(const json& value, SourceCoordinate& coordinate);
NEOGRAPH_PROGRAM_API void to_json(json& value, const DiagnosticHelp& help);
NEOGRAPH_PROGRAM_API void from_json(const json& value, DiagnosticHelp& help);
/// Writes "help" only when the diagnostic carries it.
NEOGRAPH_PROGRAM_API void to_json(json& value, const Diagnostic& diagnostic);
NEOGRAPH_PROGRAM_API void from_json(const json& value, Diagnostic& diagnostic);

/// A Program diagnostic raised at runtime. It is also a
/// graph::ConfigurationFailure: inside a Core node, the node failure
/// classifier treats it as NG.ConfigError, never retried and never routed.
class NEOGRAPH_PROGRAM_API ProgramDiagnosticError : public std::runtime_error,
                                                    public graph::ConfigurationFailure {
public:
    explicit ProgramDiagnosticError(Diagnostic diagnostic);
    const Diagnostic& diagnostic() const noexcept;

private:
    Diagnostic diagnostic_;
};

}  // namespace neograph::program
