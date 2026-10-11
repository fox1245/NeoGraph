// The internal text-safety helper: which code points are unsafe, how ill-formed
// UTF-8 is rejected or replaced, stripping, and truncation on code-point
// boundaries. #362 metadata validation and the #364 free-text opt-in use it.

#include "../src/core/text_safety.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace neograph::detail;

namespace {

std::string utf8(char32_t code_point) {
    std::string out;
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
    return out;
}

std::string bytes(std::initializer_list<unsigned> values) {
    std::string out;
    for (const auto value : values) out.push_back(static_cast<char>(value));
    return out;
}

const std::string kReplacement(kUtf8ReplacementCharacter);

// The approved lists (#362), written out independently of the tables.
std::optional<UnsafeTextClass> expected_class(char32_t cp) {
    if (cp <= 0x1F && cp != 0x09 && cp != 0x0A && cp != 0x0D) return UnsafeTextClass::C0Control;
    if (cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E) ||
        (cp >= 0x2066 && cp <= 0x2069))
        return UnsafeTextClass::BidiControl;
    if ((cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF)
        return UnsafeTextClass::ZeroWidth;
    if (cp >= 0xE0000 && cp <= 0xE007F) return UnsafeTextClass::Tag;
    return std::nullopt;
}

bool is_surrogate(char32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }

}  // namespace

TEST(TextSafetyTables, EachClassHasASortedDisjointTable) {
    std::vector<CodePointRange> all;
    for (const auto text_class : {UnsafeTextClass::C0Control, UnsafeTextClass::BidiControl,
                                  UnsafeTextClass::ZeroWidth, UnsafeTextClass::Tag}) {
        const auto table = unsafe_text_ranges(text_class);
        ASSERT_FALSE(table.empty()) << to_string(text_class);
        for (std::size_t index = 0; index < table.size(); ++index) {
            EXPECT_LE(table[index].first, table[index].last);
            if (index > 0) EXPECT_GT(table[index].first, table[index - 1].last + 1);
            all.push_back(table[index]);
        }
    }
    std::sort(all.begin(), all.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (std::size_t index = 1; index < all.size(); ++index)
        EXPECT_GT(all[index].first, all[index - 1].last) << "classes overlap";
}

TEST(TextSafetyTables, ClassificationMatchesTheApprovedListsForEveryCodePoint) {
    std::size_t unsafe = 0;
    for (char32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        const auto expected = expected_class(cp);
        const auto actual   = classify_unsafe_code_point(cp);
        if (expected != actual) {
            ADD_FAILURE() << format_code_point(cp);
            break;
        }
        if (actual) ++unsafe;
    }
    // 29 C0 + 12 bidi + 5 zero-width + 128 tag.
    EXPECT_EQ(unsafe, 29U + 12U + 5U + 128U);
}

TEST(TextSafetyTables, TabNewlineCarriageReturnDeleteAndC1AreAllowed) {
    for (const char32_t cp : {U'\t', U'\n', U'\r', U' ', char32_t{0x7F}, char32_t{0x80},
                              char32_t{0x9F}, char32_t{0x200A}, char32_t{0x2061},
                              char32_t{0xFFFD}, char32_t{0xE0080}, char32_t{0x10FFFF}})
        EXPECT_FALSE(classify_unsafe_code_point(cp).has_value()) << format_code_point(cp);
}

TEST(TextSafetyTables, NamesAndCodePointSpellingsAreStable) {
    EXPECT_EQ(to_string(UnsafeTextClass::C0Control), "c0_control");
    EXPECT_EQ(to_string(UnsafeTextClass::BidiControl), "bidi_control");
    EXPECT_EQ(to_string(UnsafeTextClass::ZeroWidth), "zero_width");
    EXPECT_EQ(to_string(UnsafeTextClass::Tag), "tag");
    EXPECT_EQ(format_code_point(0x0), "U+0000");
    EXPECT_EQ(format_code_point(0x202E), "U+202E");
    EXPECT_EQ(format_code_point(0xFEFF), "U+FEFF");
    EXPECT_EQ(format_code_point(0xE0001), "U+E0001");
    EXPECT_EQ(format_code_point(0x10FFFF), "U+10FFFF");
}

TEST(TextSafetyDetect, ReportsEachUnsafeCodePointWithItsOffsetAndClass) {
    const std::string text = "a" + utf8(0x202E) + "b\x01" + utf8(0x200D) + utf8(0x1F600) +
                             utf8(0xE0041) + "\t\n\r" + utf8(0x061C);
    const auto found = find_unsafe_code_points(text);
    const std::vector<UnsafeCodePoint> expected{
        {1, 0x202E, UnsafeTextClass::BidiControl},
        {5, 0x01, UnsafeTextClass::C0Control},
        {6, 0x200D, UnsafeTextClass::ZeroWidth},
        {13, 0xE0041, UnsafeTextClass::Tag},
        {20, 0x061C, UnsafeTextClass::BidiControl},
    };
    EXPECT_EQ(found, expected);
    EXPECT_TRUE(contains_unsafe_code_points(text));
    EXPECT_FALSE(contains_unsafe_code_points("plain text\twith\nbreaks\r" + utf8(0x1F600)));
    EXPECT_TRUE(find_unsafe_code_points("").empty());
}

TEST(TextSafetyDetect, EveryTableEntryIsFoundInsideText) {
    for (const auto text_class : {UnsafeTextClass::C0Control, UnsafeTextClass::BidiControl,
                                  UnsafeTextClass::ZeroWidth, UnsafeTextClass::Tag}) {
        for (const auto& range : unsafe_text_ranges(text_class)) {
            for (char32_t cp = range.first; cp <= range.last; ++cp) {
                const auto found = find_unsafe_code_points("x" + utf8(cp) + "y");
                ASSERT_EQ(found.size(), 1U) << format_code_point(cp);
                EXPECT_EQ(found[0], (UnsafeCodePoint{1, cp, text_class}));
            }
        }
    }
}

TEST(TextSafetyUtf8, RejectNamesTheFirstIllFormedByte) {
    const std::vector<std::pair<std::string, std::size_t>> cases{
        {bytes({0x80}), 0},                          // stray continuation
        {"ok" + bytes({0xC0, 0xAF}), 2},             // overlong two-byte
        {bytes({0xC1, 0xBF}), 0},                    // overlong two-byte
        {bytes({0xE0, 0x80, 0xAF}), 0},              // overlong three-byte
        {bytes({0xED, 0xA0, 0x80}), 0},              // encoded surrogate
        {bytes({0xF0, 0x80, 0x80, 0xAF}), 0},        // overlong four-byte
        {bytes({0xF4, 0x90, 0x80, 0x80}), 0},        // above U+10FFFF
        {bytes({0xF5, 0x80, 0x80, 0x80}), 0},        // invalid lead
        {"abc" + bytes({0xE2, 0x82}), 3},            // truncated at the end
        {"a" + bytes({0xE2, 0x82}) + "b", 1},        // truncated in the middle
        {utf8(0x1F600) + bytes({0xF0, 0x9F}), 4},
        {bytes({0xFF}), 0},
    };
    for (const auto& [text, offset] : cases) {
        EXPECT_FALSE(is_valid_utf8(text));
        try {
            (void)find_unsafe_code_points(text);
            ADD_FAILURE() << "accepted ill-formed input";
        } catch (const InvalidUtf8Error& error) {
            EXPECT_EQ(error.byte_offset(), offset);
        }
        EXPECT_THROW((void)contains_unsafe_code_points(text), InvalidUtf8Error);
        EXPECT_THROW((void)strip_unsafe_code_points(text, InvalidUtf8Policy::Reject),
                     InvalidUtf8Error);
        EXPECT_THROW((void)truncate_utf8(text, 64, InvalidUtf8Policy::Reject), InvalidUtf8Error);
    }
}

TEST(TextSafetyUtf8, BoundaryCodePointsAreWellFormed) {
    for (const char32_t cp : {char32_t{0x0}, char32_t{0x7F}, char32_t{0x80}, char32_t{0x7FF},
                              char32_t{0x800}, char32_t{0xD7FF}, char32_t{0xE000},
                              char32_t{0xFFFF}, char32_t{0x10000}, char32_t{0x10FFFF}}) {
        EXPECT_TRUE(is_valid_utf8(utf8(cp))) << format_code_point(cp);
        EXPECT_EQ(replace_invalid_utf8(utf8(cp)), utf8(cp));
    }
}

TEST(TextSafetyUtf8, ReplaceSubstitutesOneReplacementPerMaximalSubpart) {
    // Unicode chapter 3, "U+FFFD Substitution of Maximal Subparts".
    EXPECT_EQ(replace_invalid_utf8(bytes({0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2, 0x62, 0x80,
                                          0x63, 0x80, 0xBF, 0x64})),
              "a" + kReplacement + kReplacement + kReplacement + "b" + kReplacement + "c" +
                  kReplacement + kReplacement + "d");
    const std::vector<std::pair<std::string, std::string>> cases{
        {bytes({0xC0, 0xAF}), kReplacement + kReplacement},
        {bytes({0xE0, 0x80, 0xAF}), kReplacement + kReplacement + kReplacement},
        {bytes({0xED, 0xA0, 0x80}), kReplacement + kReplacement + kReplacement},
        {bytes({0xF4, 0x90, 0x80, 0x80}),
         kReplacement + kReplacement + kReplacement + kReplacement},
        {bytes({0xE2, 0x82}), kReplacement},
        {bytes({0xF0, 0x9F, 0x98}), kReplacement},
        {"a" + bytes({0xE2, 0x82}) + "b", "a" + kReplacement + "b"},
        {utf8(0x1F600) + bytes({0xF0, 0x9F}), utf8(0x1F600) + kReplacement},
    };
    for (const auto& [text, expected] : cases) EXPECT_EQ(replace_invalid_utf8(text), expected);
}

TEST(TextSafetyStrip, RemovesEveryUnsafeCodePointAndKeepsTheRest) {
    const std::string text = "pay" + utf8(0x202E) + "lpd." + utf8(0x200B) + "exe\x07\t" +
                             utf8(0xE0049) + utf8(0x1F468) + utf8(0x200D) + utf8(0x1F469) +
                             utf8(0xFEFF) + "\n";
    EXPECT_EQ(strip_unsafe_code_points(text, InvalidUtf8Policy::Reject),
              "paylpd.exe\t" + utf8(0x1F468) + utf8(0x1F469) + "\n");
    EXPECT_EQ(strip_unsafe_code_points("", InvalidUtf8Policy::Reject), "");
}

TEST(TextSafetyStrip, ReplacePolicyRepairsBeforeStripping) {
    const std::string text = "a" + bytes({0xE2, 0x82}) + utf8(0x202E) + "b" + bytes({0x80});
    EXPECT_EQ(strip_unsafe_code_points(text, InvalidUtf8Policy::Replace),
              "a" + kReplacement + "b" + kReplacement);
    EXPECT_THROW((void)strip_unsafe_code_points(text, InvalidUtf8Policy::Reject),
                 InvalidUtf8Error);
}

TEST(TextSafetyTruncate, NeverSplitsACodePoint) {
    const std::string text = "a" + utf8(0xE9) + utf8(0x20AC) + utf8(0x1F600) + "z";  // 1+2+3+4+1
    const std::vector<std::size_t> boundaries{0, 1, 3, 6, 10, 11};
    for (std::size_t max = 0; max <= text.size() + 2; ++max) {
        const auto cut = truncate_utf8(text, max, InvalidUtf8Policy::Reject);
        const auto expected =
            *std::prev(std::upper_bound(boundaries.begin(), boundaries.end(), max));
        EXPECT_EQ(cut, text.substr(0, expected)) << "max_bytes " << max;
        EXPECT_TRUE(is_valid_utf8(cut));
        EXPECT_LE(cut.size(), max);
    }
}

TEST(TextSafetyTruncate, ReplacePolicyMeasuresTheRepairedText) {
    // The ill-formed byte becomes a three-byte U+FFFD before the cap applies.
    const std::string text = "ab" + bytes({0x80}) + "cd";
    EXPECT_EQ(truncate_utf8(text, 4, InvalidUtf8Policy::Replace), "ab");
    EXPECT_EQ(truncate_utf8(text, 5, InvalidUtf8Policy::Replace), "ab" + kReplacement);
}

TEST(TextSafetySanitize, StripsThenTruncatesToTheByteCap) {
    std::string text;
    for (int index = 0; index < 200; ++index) text += utf8(0x20AC) + utf8(0x202E);
    const auto sanitized = sanitize_untrusted_text(text, 512);
    EXPECT_EQ(sanitized.size(), 510U);  // 170 euro signs; the 171st would cross 512
    EXPECT_FALSE(contains_unsafe_code_points(sanitized));
    EXPECT_EQ(sanitize_untrusted_text("ok" + bytes({0xC0}), 512), "ok" + kReplacement);
    EXPECT_THROW((void)sanitize_untrusted_text("ok" + bytes({0xC0}), 512,
                                               InvalidUtf8Policy::Reject),
                 InvalidUtf8Error);
}

TEST(TextSafetySanitize, RandomBytesAlwaysYieldSafeWellFormedBoundedText) {
    std::mt19937 rng(20261011);
    // Bias toward bytes that build multi-byte, unsafe and ill-formed sequences.
    const std::vector<unsigned> alphabet{0x00, 0x01, 0x09, 0x0A, 0x1F, 0x20, 0x41, 0x7F, 0x80,
                                         0x8F, 0x9F, 0xA0, 0xAA, 0xBF, 0xC0, 0xC2, 0xD8, 0xDF,
                                         0xE0, 0xE2, 0xED, 0xEF, 0xF0, 0xF3, 0xF4, 0xF5, 0xFF,
                                         0x80, 0x81, 0x88, 0xA8, 0xAE, 0xBB, 0xB8};
    for (int round = 0; round < 20000; ++round) {
        std::string text;
        const auto  length = rng() % 24;
        for (unsigned index = 0; index < length; ++index)
            text.push_back(static_cast<char>(alphabet[rng() % alphabet.size()]));
        const auto max       = static_cast<std::size_t>(rng() % 32);
        const auto sanitized = sanitize_untrusted_text(text, max);
        ASSERT_TRUE(is_valid_utf8(sanitized)) << round;
        ASSERT_FALSE(contains_unsafe_code_points(sanitized)) << round;
        ASSERT_LE(sanitized.size(), max) << round;
        const auto repaired = replace_invalid_utf8(text);
        ASSERT_TRUE(is_valid_utf8(repaired));
        ASSERT_EQ(replace_invalid_utf8(repaired), repaired);
        ASSERT_EQ(is_valid_utf8(text), repaired == text);
        const auto stripped = strip_unsafe_code_points(text, InvalidUtf8Policy::Replace);
        ASSERT_EQ(strip_unsafe_code_points(stripped, InvalidUtf8Policy::Reject), stripped);
        // The result is a prefix of the stripped text: truncation never reorders or alters.
        ASSERT_EQ(stripped.compare(0, sanitized.size(), sanitized), 0) << round;
    }
}

TEST(TextSafetyUtf8, SurrogatesAndOutOfRangeValuesAreNeverDecoded) {
    // Every three-byte ED A0..BF sequence is a surrogate and must be rejected.
    for (unsigned second = 0xA0; second <= 0xBF; ++second) {
        const auto text = bytes({0xED, second, 0x80});
        EXPECT_FALSE(is_valid_utf8(text));
    }
    for (char32_t cp = 0xD7F0; cp <= 0xE010; ++cp) {
        if (is_surrogate(cp)) continue;
        EXPECT_TRUE(is_valid_utf8(utf8(cp))) << format_code_point(cp);
    }
}
