#include <gtest/gtest.h>

#include "../examples/cookbook/self_evolving_chatbot/utf8_prefix.h"

#include <string>
#include <string_view>

namespace {

// Structural UTF-8 check (lead byte + exact number of continuation bytes).
bool well_formed(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = byte < 0x80 ? 1 : (byte >> 5) == 0x6 ? 2 : (byte >> 4) == 0xE ? 3 : (byte >> 3) == 0x1E ? 4 : 0;
        if (length == 0 || i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        i += length;
    }
    return true;
}

std::size_t sequence_length(unsigned char lead) {
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

}  // namespace

TEST(ExampleUtf8Prefix, ReturnsInputUnchangedWhenItFits) {
    EXPECT_EQ(examples::utf8_prefix("", 0), "");
    EXPECT_EQ(examples::utf8_prefix("abc", 3), "abc");
    EXPECT_EQ(examples::utf8_prefix("abc", 100), "abc");
    // Fits exactly: the trailing multi-byte sequence is complete, not cut.
    EXPECT_EQ(examples::utf8_prefix("a\xE2\x82\xAC", 4), "a\xE2\x82\xAC");
}

TEST(ExampleUtf8Prefix, NeverEndsInsideASequenceAndKeepsEveryWholeOne) {
    // 1-, 2-, 3- and 4-byte code points in one string.
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z\xED\x95\x9C";
    ASSERT_TRUE(well_formed(text));
    for (std::size_t budget = 0; budget <= text.size(); ++budget) {
        const auto prefix = examples::utf8_prefix(text, budget);
        EXPECT_LE(prefix.size(), budget) << budget;
        EXPECT_EQ(prefix, std::string_view(text).substr(0, prefix.size())) << budget;
        EXPECT_TRUE(well_formed(prefix)) << budget;
        // Maximal: the next code point would not have fit in the budget.
        if (prefix.size() < text.size())
            EXPECT_GT(prefix.size() + sequence_length(static_cast<unsigned char>(text[prefix.size()])), budget) << budget;
    }
}

TEST(ExampleUtf8Prefix, ZeroBudgetIsEmptyEvenInsideAMultiByteLead) {
    EXPECT_EQ(examples::utf8_prefix("\xC3\xA9", 0), "");
    EXPECT_EQ(examples::utf8_prefix("\xC3\xA9", 1), "");
}

TEST(ExampleUtf8Prefix, ByteTwoHundredCutOfKoreanTextBecomesWellFormed) {
    // The judge history cut: 3-byte Hangul syllables put byte 200 inside a character.
    std::string text;
    for (int i = 0; i < 100; ++i) text += "\xED\x95\x9C";
    ASSERT_FALSE(well_formed(text.substr(0, 200)));
    const auto prefix = examples::utf8_prefix(text, 200);
    EXPECT_EQ(prefix.size(), 198U);
    EXPECT_TRUE(well_formed(prefix));
}

TEST(ExampleUtf8Prefix, InvalidInputStopsBeforeTheMalformedCodePoint) {
    EXPECT_EQ(examples::utf8_prefix(std::string_view("\x80\x80\x80", 3), 3), "");
    EXPECT_EQ(examples::utf8_prefix("ab\xE2\x82" "cd", 4), "ab");
    EXPECT_EQ(examples::utf8_prefix("ab\xE2" "cd", 5), "ab");
    EXPECT_EQ(examples::utf8_prefix("a\xFF" "b", 3), "a");
    EXPECT_EQ(examples::utf8_prefix("a\xC0\xAF", 3), "a");       // overlong two-byte
    EXPECT_EQ(examples::utf8_prefix("a\xE0\x80\x80", 4), "a");   // overlong three-byte
    EXPECT_EQ(examples::utf8_prefix("a\xED\xA0\x80", 4), "a");   // surrogate
    EXPECT_EQ(examples::utf8_prefix("a\xF0\x80\x80\x80", 5), "a");// overlong four-byte
    EXPECT_EQ(examples::utf8_prefix("a\xF4\x90\x80\x80", 5), "a");// beyond U+10FFFF
    EXPECT_EQ(examples::utf8_prefix("a\xF5\x80\x80\x80", 5), "a");
    EXPECT_EQ(examples::utf8_prefix("ab\xE2\x82", 100), "ab");    // incomplete original
}

TEST(ExampleUtf8Prefix, AcceptsUnicodeEncodingBoundaryCodePoints) {
    const std::string text =
        "\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF"
        "\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF";
    EXPECT_EQ(examples::utf8_prefix(text, text.size()), text);
}
