#include "text_safety.h"

#include <array>
#include <cstdio>

namespace neograph::detail {
namespace {

constexpr std::array<CodePointRange, 3> kC0Controls{{
    {0x0000, 0x0008},
    {0x000B, 0x000C},
    {0x000E, 0x001F},
}};

constexpr std::array<CodePointRange, 4> kBidiControls{{
    {0x061C, 0x061C},  // ARABIC LETTER MARK
    {0x200E, 0x200F},  // LEFT-TO-RIGHT MARK, RIGHT-TO-LEFT MARK
    {0x202A, 0x202E},  // LRE, RLE, PDF, LRO, RLO
    {0x2066, 0x2069},  // LRI, RLI, FSI, PDI
}};

constexpr std::array<CodePointRange, 3> kZeroWidth{{
    {0x200B, 0x200D},  // ZERO WIDTH SPACE, ZWNJ, ZWJ
    {0x2060, 0x2060},  // WORD JOINER
    {0xFEFF, 0xFEFF},  // ZERO WIDTH NO-BREAK SPACE (BOM)
}};

constexpr std::array<CodePointRange, 1> kTags{{
    {0xE0000, 0xE007F},
}};

constexpr std::array<UnsafeTextClass, 4> kClasses{
    UnsafeTextClass::C0Control, UnsafeTextClass::BidiControl, UnsafeTextClass::ZeroWidth,
    UnsafeTextClass::Tag};

/// One step of a UTF-8 decoder. `length` is never zero. When `valid` is
/// false, `length` covers one maximal subpart of an ill-formed sequence.
struct Decoded {
    char32_t    code_point = 0;
    std::size_t length     = 1;
    bool        valid      = false;
};

bool in_range(unsigned char byte, unsigned char low, unsigned char high) noexcept {
    return byte >= low && byte <= high;
}

Decoded decode(std::string_view text, std::size_t offset) noexcept {
    const auto lead = static_cast<unsigned char>(text[offset]);
    if (lead < 0x80) return {lead, 1, true};

    std::size_t   needed = 0;
    char32_t      value  = 0;
    unsigned char low    = 0x80;  // allowed range of the second byte (Unicode table 3-7)
    unsigned char high   = 0xBF;
    if (in_range(lead, 0xC2, 0xDF)) {
        needed = 1;
        value  = lead & 0x1F;
    } else if (in_range(lead, 0xE0, 0xEF)) {
        needed = 2;
        value  = lead & 0x0F;
        if (lead == 0xE0) low = 0xA0;   // no overlongs
        if (lead == 0xED) high = 0x9F;  // no surrogates
    } else if (in_range(lead, 0xF0, 0xF4)) {
        needed = 3;
        value  = lead & 0x07;
        if (lead == 0xF0) low = 0x90;   // no overlongs
        if (lead == 0xF4) high = 0x8F;  // nothing above U+10FFFF
    } else {
        return {};  // a continuation byte, C0/C1 (overlong) or F5..FF as a lead
    }

    std::size_t length = 1;
    for (std::size_t index = 0; index < needed; ++index) {
        if (offset + length >= text.size()) return {0, length, false};
        const auto byte = static_cast<unsigned char>(text[offset + length]);
        const auto min  = index == 0 ? low : static_cast<unsigned char>(0x80);
        const auto max  = index == 0 ? high : static_cast<unsigned char>(0xBF);
        if (!in_range(byte, min, max)) return {0, length, false};
        value = (value << 6) | (byte & 0x3F);
        ++length;
    }
    return {value, length, true};
}

void append_utf8(std::string& out, std::string_view text, std::size_t offset, std::size_t length) {
    out.append(text.data() + offset, length);
}

/// Ill-formed input under `policy`: Reject throws, Replace returns the repaired text.
std::string well_formed(std::string_view text, InvalidUtf8Policy policy) {
    if (policy == InvalidUtf8Policy::Replace) return replace_invalid_utf8(text);
    for (std::size_t offset = 0; offset < text.size();) {
        const auto step = decode(text, offset);
        if (!step.valid) throw InvalidUtf8Error(offset);
        offset += step.length;
    }
    return std::string(text);
}

}  // namespace

InvalidUtf8Error::InvalidUtf8Error(std::size_t byte_offset)
    : std::invalid_argument("text is not well-formed UTF-8 at byte " +
                            std::to_string(byte_offset)),
      byte_offset_(byte_offset) {}

std::span<const CodePointRange> unsafe_text_ranges(UnsafeTextClass text_class) noexcept {
    switch (text_class) {
        case UnsafeTextClass::C0Control:
            return kC0Controls;
        case UnsafeTextClass::BidiControl:
            return kBidiControls;
        case UnsafeTextClass::ZeroWidth:
            return kZeroWidth;
        case UnsafeTextClass::Tag:
            return kTags;
    }
    return {};
}

std::string_view to_string(UnsafeTextClass text_class) noexcept {
    switch (text_class) {
        case UnsafeTextClass::C0Control:
            return "c0_control";
        case UnsafeTextClass::BidiControl:
            return "bidi_control";
        case UnsafeTextClass::ZeroWidth:
            return "zero_width";
        case UnsafeTextClass::Tag:
            return "tag";
    }
    return "unknown";
}

std::optional<UnsafeTextClass> classify_unsafe_code_point(char32_t code_point) noexcept {
    // Fast path: everything printable in ASCII, and the three allowed C0 controls.
    if (code_point >= 0x20 && code_point < 0x061C) return std::nullopt;
    for (const auto text_class : kClasses) {
        for (const auto& range : unsafe_text_ranges(text_class)) {
            if (code_point >= range.first && code_point <= range.last) return text_class;
        }
    }
    return std::nullopt;
}

std::string format_code_point(char32_t code_point) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<unsigned>(code_point));
    return buffer;
}

bool is_valid_utf8(std::string_view text) noexcept {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto step = decode(text, offset);
        if (!step.valid) return false;
        offset += step.length;
    }
    return true;
}

std::vector<UnsafeCodePoint> find_unsafe_code_points(std::string_view text) {
    std::vector<UnsafeCodePoint> found;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto step = decode(text, offset);
        if (!step.valid) throw InvalidUtf8Error(offset);
        if (const auto text_class = classify_unsafe_code_point(step.code_point))
            found.push_back({offset, step.code_point, *text_class});
        offset += step.length;
    }
    return found;
}

bool contains_unsafe_code_points(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto step = decode(text, offset);
        if (!step.valid) throw InvalidUtf8Error(offset);
        if (classify_unsafe_code_point(step.code_point)) return true;
        offset += step.length;
    }
    return false;
}

std::string replace_invalid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto step = decode(text, offset);
        if (step.valid)
            append_utf8(out, text, offset, step.length);
        else
            out.append(kUtf8ReplacementCharacter);
        offset += step.length;
    }
    return out;
}

std::string strip_unsafe_code_points(std::string_view text, InvalidUtf8Policy policy) {
    const auto  input = well_formed(text, policy);
    std::string out;
    out.reserve(input.size());
    for (std::size_t offset = 0; offset < input.size();) {
        const auto step = decode(input, offset);
        if (!classify_unsafe_code_point(step.code_point))
            append_utf8(out, input, offset, step.length);
        offset += step.length;
    }
    return out;
}

std::string truncate_utf8(std::string_view text, std::size_t max_bytes, InvalidUtf8Policy policy) {
    auto input = well_formed(text, policy);
    if (input.size() <= max_bytes) return input;
    std::size_t cut = max_bytes;
    // Back off over continuation bytes: the cut must land on a lead byte.
    while (cut > 0 && (static_cast<unsigned char>(input[cut]) & 0xC0) == 0x80) --cut;
    input.resize(cut);
    return input;
}

std::string sanitize_untrusted_text(std::string_view  text,
                                    std::size_t       max_bytes,
                                    InvalidUtf8Policy policy) {
    return truncate_utf8(strip_unsafe_code_points(text, policy), max_bytes,
                         InvalidUtf8Policy::Reject);
}

}  // namespace neograph::detail
