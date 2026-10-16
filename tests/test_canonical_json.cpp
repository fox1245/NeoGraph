#include "../src/core/canonical_json.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using neograph::json;
using neograph::detail::canonical_json_bytes;
using neograph::detail::parse_json_strict;

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

// The json-handle walk canonical_json_bytes used before #469, kept as the
// oracle for its yyjson walk. Floats go through the library: their encoding
// did not change.
void oracle_append(std::string& out, const json& value) {
    if (value.is_null()) {
        out += "null";
    } else if (value.is_boolean()) {
        out += value.get<bool>() ? "true" : "false";
    } else if (value.is_number_unsigned()) {
        out += std::to_string(value.get<unsigned long long>());
    } else if (value.is_number_integer()) {
        out += std::to_string(value.get<long long>());
    } else if (value.is_number_float()) {
        out += canonical_json_bytes(json(value.get<double>()));
    } else if (value.is_string()) {
        out += reference_escape(value.get<std::string>());
    } else if (value.is_array()) {
        out += '[';
        bool first = true;
        for (const auto element : value) {
            if (!first) out += ',';
            first = false;
            oracle_append(out, element);
        }
        out += ']';
    } else {
        std::vector<std::pair<std::string, json>> fields;
        for (const auto& [key, child] : value.items()) fields.emplace_back(key, child);
        std::sort(fields.begin(), fields.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        out += '{';
        bool first = true;
        for (const auto& [key, child] : fields) {
            if (!first) out += ',';
            first = false;
            out += reference_escape(key);
            out += ':';
            oracle_append(out, child);
        }
        out += '}';
    }
}

std::string oracle(const json& value) {
    std::string out;
    oracle_append(out, value);
    return out;
}

// Keys and strings share prefixes, need escapes, mix scripts and sort by byte
// above 0x7f, where signed and unsigned char order differ.
const std::vector<std::string>& sample_strings() {
    static const std::vector<std::string> values{
        "", "a", "ab", "a\"b", "a\\b", "line\nbreak", "tab\tend", "\x01" "ctl", "\x1f",
        "z", "Z", "_", "\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\xea\xb0\x80",
        "\xe6\x97\xa5\xe6\x9c\xac", "a\xc3\xa9", "long string past one word boundary"};
    return values;
}

json random_value(std::mt19937_64& random, int depth) {
    const auto& strings = sample_strings();
    const auto  pick    = [&](std::size_t n) { return static_cast<std::size_t>(random() % n); };
    const auto  kind    = depth >= 5 ? pick(7) : pick(9);
    switch (kind) {
        case 0: return json(nullptr);
        case 1: return json(random() % 2 == 0);
        case 2: return json(static_cast<long long>(random()) >> pick(64));
        case 3: return json(static_cast<unsigned long long>(random()) >> pick(64));
        case 4: {
            static const std::array<double, 9> doubles{0.0,   -0.0,   0.1,     -2.5,
                                                       1e300, 1e-300, 123.456, 6.02214076e23,
                                                       -1.0 / 3.0};
            return json(doubles[pick(doubles.size())]);
        }
        case 5: return json(static_cast<int>(random()));
        case 6: return json(strings[pick(strings.size())]);
        case 7: {
            auto array = json::array();
            for (std::size_t index = pick(5); index > 0; --index)
                array.push_back(random_value(random, depth + 1));
            return array;
        }
        default: {
            auto object = json::object();
            for (std::size_t index = pick(6); index > 0; --index)
                object[strings[pick(strings.size())]] = random_value(random, depth + 1);
            return object;
        }
    }
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

TEST(CanonicalJsonTest, YyjsonWalkMatchesJsonHandleWalkOnRandomTrees) {
    std::mt19937_64 random(469);
    for (int round = 0; round < 2000; ++round) {
        const auto value = random_value(random, 0);
        const auto bytes = canonical_json_bytes(value);
        ASSERT_EQ(bytes, oracle(value)) << "round " << round;
        // Canonical bytes parse back to themselves.
        ASSERT_EQ(canonical_json_bytes(parse_json_strict(bytes)), bytes) << "round " << round;
    }
}

TEST(CanonicalJsonTest, ParsedNumbersKeepTheirIntegerAndFloatEncodings) {
    const std::string text =
        R"({"big":18446744073709551615,"min":-9223372036854775808,"neg":-1,"one":1,)"
        R"("real":1.5,"zero":0})";
    EXPECT_EQ(canonical_json_bytes(json::parse(text)), text);
    EXPECT_EQ(canonical_json_bytes(json::parse(text)), oracle(json::parse(text)));
}

TEST(CanonicalJsonTest, DuplicateMembersAreReportedInWalkOrder) {
    // A duplicate inside an earlier member is found before a later duplicate.
    const auto value = json::parse(R"({"b":{"x":1,"x":2},"a":1,"a":2})");
    try {
        (void)parse_json_strict(R"({"b":{"x":1,"x":2},"a":1,"a":2})");
        FAIL() << "duplicate fields were accepted";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("duplicate object field 'x'"), std::string::npos)
            << error.what();
    }
    try {
        (void)canonical_json_bytes(value);
        FAIL() << "duplicate member names were serialized";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("duplicate object member names"),
                  std::string::npos)
            << error.what();
    }
}

TEST(CanonicalJsonTest, NestingDepthLimitIsUnchanged) {
    auto nested = [](int depth) {
        auto value = json::array();
        for (int level = 1; level < depth; ++level) {
            auto outer = json::array();
            outer.push_back(std::move(value));
            value = std::move(outer);
        }
        return value;
    };
    EXPECT_NO_THROW((void)canonical_json_bytes(nested(256)));
    EXPECT_THROW((void)canonical_json_bytes(nested(257)), std::invalid_argument);
    auto object = json::object();
    object["inner"] = nested(256);
    EXPECT_THROW((void)canonical_json_bytes(object), std::invalid_argument);
}

TEST(CanonicalJsonTest, InvalidUtf8InANestedKeyIsRejected) {
    auto inner = json::object();
    inner[std::string("ok\xff")] = 1;
    auto value = json::object();
    value["outer"] = inner;
    EXPECT_THROW((void)canonical_json_bytes(value), std::invalid_argument);
}
