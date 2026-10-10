#pragma once

#include <cstddef>
#include <string_view>

namespace examples {

// Longest valid UTF-8 prefix within max_bytes. Stop before malformed input or
// an incomplete code point, including when the original text fits the budget.
// Reject overlong encodings, surrogates and code points beyond U+10FFFF.
inline std::string_view utf8_prefix(std::string_view text, std::size_t max_bytes) noexcept {
    const auto limit = text.size() < max_bytes ? text.size() : max_bytes;
    std::size_t offset = 0;
    while (offset < limit) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        const std::size_t length = lead < 0x80 ? 1 :
            lead >= 0xC2 && lead <= 0xDF ? 2 :
            lead >= 0xE0 && lead <= 0xEF ? 3 :
            lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        if (length == 0 || length > limit - offset) break;
        bool valid = true;
        for (std::size_t i = 1; i < length; ++i) {
            const auto byte = static_cast<unsigned char>(text[offset + i]);
            if (byte < 0x80 || byte > 0xBF ||
                (i == 1 && ((lead == 0xE0 && byte < 0xA0) ||
                            (lead == 0xED && byte > 0x9F) ||
                            (lead == 0xF0 && byte < 0x90) ||
                            (lead == 0xF4 && byte > 0x8F)))) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        offset += length;
    }
    return text.substr(0, offset);
}

} // namespace examples
