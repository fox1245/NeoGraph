#include "../src/core/canonical_json.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using neograph::json;
using neograph::detail::canonical_json_bytes;

std::string reference_escape(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) {
                    result += "\\u00";
                    result += hex[c >> 4];
                    result += hex[c & 15];
                } else result += static_cast<char>(c);
        }
    }
    return result + '"';
}
}

TEST(CanonicalJsonTest, AsciiAndEscapeBytesMatchScalarOracleAcrossWordBoundaries) {
    for (unsigned c = 0; c < 128; ++c) {
        for (std::size_t prefix = 0; prefix < 24; ++prefix) {
            for (std::size_t suffix : {0U, 1U, 7U, 8U, 17U}) {
                const auto value = std::string(prefix, 'x') + static_cast<char>(c) + std::string(suffix, 'y');
                ASSERT_EQ(canonical_json_bytes(json(value)), reference_escape(value))
                    << "byte=" << c << " prefix=" << prefix << " suffix=" << suffix;
            }
        }
    }
}

TEST(CanonicalJsonTest, UnicodeAndEscapesMatchScalarOracleAcrossWordBoundaries) {
    const std::array<std::string, 7> units{
        "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf",
        "\xed\x95\x9c\xea\xb8\x80", "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf"};
    for (const auto& unit : units) {
        for (std::size_t prefix = 0; prefix < 24; ++prefix) {
            const auto value = std::string(prefix, 'a') + unit + "\"\\\n" + unit + std::string(25, 'z');
            EXPECT_EQ(canonical_json_bytes(json(value)), reference_escape(value));
            const auto object = json{{value, value}, {"plain", "value"}};
            const auto encoded = canonical_json_bytes(object);
            EXPECT_EQ(canonical_json_bytes(neograph::detail::parse_json_strict(encoded)), encoded);
        }
    }
}

TEST(CanonicalJsonTest, MixedUnicodeCorpusMatchesIndependentEscaping) {
    const std::array<std::string, 8> tokens{
        "\xed\x95\x9c", "\xf0\x9f\x98\x80", "\xc2\xa0", "\xe2\x80\xa8",
        "\"", "\\", "\n", std::string(1, '\0')};
    std::uint32_t random = 0x519712u;
    for (std::size_t case_id = 0; case_id < 4096; ++case_id) {
        std::string value;
        for (std::size_t i = 0; i < case_id % 127; ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            if ((random & 15) < 8) value += tokens[random & 7];
            else value += static_cast<char>(random & 127);
        }
        ASSERT_EQ(canonical_json_bytes(json(value)), reference_escape(value)) << case_id;
    }
}

TEST(CanonicalJsonTest, InvalidUtf8CannotHideAfterAsciiWords) {
    const std::vector<std::string> invalid{
        "\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xc2", "\xc2x",
        "\xe0\x80\x80", "\xe2\x82", "\xe2x\xac", "\xed\xa0\x80",
        "\xf0\x80\x80\x80", "\xf0\x9f\x98", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"};
    for (const auto& bytes : invalid) {
        for (std::size_t prefix = 0; prefix < 24; ++prefix) {
            const auto value = std::string(prefix, 'a') + bytes + "suffix";
            EXPECT_THROW((void)canonical_json_bytes(json(value)), std::invalid_argument);
            const auto unaligned = "x" + value;
            EXPECT_THROW(neograph::detail::validate_utf8(std::string_view(unaligned).substr(1)),
                         std::invalid_argument);
        }
    }
}

TEST(CanonicalJsonTest, ShortAndUnalignedUtf8ViewsStayWithinBounds) {
    for (std::size_t offset = 0; offset < 16; ++offset) {
        for (std::size_t length = 0; length < 33; ++length) {
            const std::string buffer(offset + length, 'x');
            const auto view = std::string_view(buffer).substr(offset);
            EXPECT_NO_THROW(neograph::detail::validate_utf8(view));
            EXPECT_EQ(canonical_json_bytes(json(std::string(view))), reference_escape(view));
        }
    }
}

TEST(CanonicalJsonTest, ConservativeMaterializedLimitIsUnchanged) {
    constexpr std::size_t maximum = 16u * 1024u * 1024u;
    EXPECT_EQ(canonical_json_bytes(json(std::string(maximum - 2, 'x'))).size(), maximum);
    EXPECT_THROW((void)canonical_json_bytes(json(std::string(maximum - 1, 'x'))), std::invalid_argument);
    const std::size_t controls = (maximum - 2) / 6;
    EXPECT_NO_THROW((void)canonical_json_bytes(json(std::string(controls, '\n'))));
    EXPECT_THROW((void)canonical_json_bytes(json(std::string(controls + 1, '\n'))), std::invalid_argument);
}
