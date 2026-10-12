/**
 * @file text_safety.h
 * @brief Internal: detect, strip and truncate invisible or deceptive characters.
 *
 * Text a host or a model will read later (graph metadata, failure records)
 * must not hide content. This helper is the one place that decides which code
 * points count as unsafe, so the metadata validator (an error per finding) and
 * the failure-record writer (strip, then truncate) cannot drift apart.
 *
 * Internal header: it lives under src/ and is not installed. The functions are
 * exported only so that other NeoGraph libraries can link them in shared
 * builds; they carry no compatibility promise.
 *
 * Unsafe classes, one code-point table each (see unsafe_text_ranges()):
 *
 *  - C0 controls other than TAB, LF and CR: U+0000-0008, U+000B-000C,
 *    U+000E-001F. DEL (U+007F) and the C1 controls are not C0 and are not in
 *    this class.
 *  - Bidirectional controls: U+061C, U+200E, U+200F, U+202A-202E,
 *    U+2066-2069.
 *  - Zero-width characters: U+200B-200D, U+2060, U+FEFF.
 *  - Tag characters: U+E0000-E007F.
 *
 * Stripping removes every code point in these classes, including ZWJ and
 * ZWNJ, so emoji sequences and some scripts lose their joiners. That is the
 * intended trade: the text is a record, not a rendering.
 *
 * Invalid UTF-8 (a stray continuation byte, a truncated or overlong sequence,
 * an encoded surrogate, or a value above U+10FFFF) is handled by an explicit
 * policy:
 *
 *  - Reject: throw InvalidUtf8Error, naming the byte offset of the first
 *    ill-formed sequence.
 *  - Replace: substitute U+FFFD for each maximal subpart of an ill-formed
 *    sequence, as Unicode chapter 3 ("U+FFFD Substitution of Maximal
 *    Subparts") and the WHATWG decoder do. Valid bytes are never consumed by
 *    a replacement.
 */
#pragma once

#include <neograph/api.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph::detail {

enum class UnsafeTextClass : std::uint8_t {
    C0Control,
    BidiControl,
    ZeroWidth,
    Tag,
};

/// Inclusive code-point range.
struct CodePointRange {
    char32_t first;
    char32_t last;
};

enum class InvalidUtf8Policy : std::uint8_t {
    Reject,
    Replace,
};

/// One unsafe code point found in a UTF-8 string.
struct UnsafeCodePoint {
    std::size_t     byte_offset = 0;
    char32_t        code_point  = 0;
    UnsafeTextClass text_class  = UnsafeTextClass::C0Control;

    bool operator==(const UnsafeCodePoint&) const = default;
};

/// Thrown by the Reject policy for ill-formed UTF-8.
class NEOGRAPH_API InvalidUtf8Error : public std::invalid_argument {
public:
    explicit InvalidUtf8Error(std::size_t byte_offset);
    std::size_t byte_offset() const noexcept { return byte_offset_; }

private:
    std::size_t byte_offset_;
};

/// U+FFFD encoded as UTF-8.
inline constexpr std::string_view kUtf8ReplacementCharacter = "\xEF\xBF\xBD";

/// The code-point table of one class, sorted and non-overlapping.
NEOGRAPH_API std::span<const CodePointRange> unsafe_text_ranges(UnsafeTextClass text_class) noexcept;

/// Stable lowercase name of a class ("c0_control", "bidi_control", "zero_width", "tag").
NEOGRAPH_API std::string_view to_string(UnsafeTextClass text_class) noexcept;

/// The class of an unsafe code point, or nullopt when it is allowed.
NEOGRAPH_API std::optional<UnsafeTextClass> classify_unsafe_code_point(char32_t code_point) noexcept;

/// "U+XXXX" with at least four uppercase hex digits, as diagnostics witness code points.
NEOGRAPH_API std::string format_code_point(char32_t code_point);

/// True when `text` is well-formed UTF-8.
NEOGRAPH_API bool is_valid_utf8(std::string_view text) noexcept;

/// Every unsafe code point in order. Throws InvalidUtf8Error on ill-formed input.
NEOGRAPH_API std::vector<UnsafeCodePoint> find_unsafe_code_points(std::string_view text);

/// Like find_unsafe_code_points(), but stops at the first finding.
NEOGRAPH_API bool contains_unsafe_code_points(std::string_view text);

/// Replace each maximal ill-formed subpart with U+FFFD; well-formed input is returned unchanged.
NEOGRAPH_API std::string replace_invalid_utf8(std::string_view text);

/// The one rule for text that crosses a boundary (tool results, MCP, gRPC,
/// Python, json::dump): `text` itself when it is well-formed UTF-8, moved
/// without a copy, otherwise replace_invalid_utf8(text).
NEOGRAPH_API std::string valid_utf8(std::string text);

/// Remove every unsafe code point. Ill-formed input follows `policy`.
NEOGRAPH_API std::string strip_unsafe_code_points(std::string_view  text,
                                                  InvalidUtf8Policy policy);

/**
 * The longest prefix of `text` that is at most `max_bytes` long and ends on a
 * code-point boundary. A code point is never split; the result may be shorter
 * than `max_bytes` by up to three bytes. Ill-formed input follows `policy`
 * (Replace substitutes before measuring).
 */
NEOGRAPH_API std::string truncate_utf8(std::string_view  text,
                                       std::size_t       max_bytes,
                                       InvalidUtf8Policy policy);

/**
 * Text from an untrusted producer made safe to store and show: ill-formed
 * sequences follow `policy`, unsafe code points are stripped, and the result
 * is truncated to `max_bytes` on a code-point boundary.
 */
NEOGRAPH_API std::string sanitize_untrusted_text(
    std::string_view  text,
    std::size_t       max_bytes,
    InvalidUtf8Policy policy = InvalidUtf8Policy::Replace);

}  // namespace neograph::detail
