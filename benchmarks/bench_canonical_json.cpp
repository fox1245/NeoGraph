// String-focused canonical JSON benchmark. Values are checked against an
// independent scalar escape oracle; preparation/verification is not timed.
#include "../src/core/canonical_json.h"

#include <charconv>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::size_t positive(std::string_view text, bool zero_allowed = false) {
    std::size_t result = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size() ||
        (!result && !zero_allowed) || result > 1024 * 1024)
        throw std::invalid_argument("number outside benchmark bounds");
    return result;
}

std::string payload(std::string_view name, std::size_t size) {
    std::string unit;
    if (name == "ascii") unit = "ordinary-ascii-0123456789";
    else if (name == "unicode") unit = "\xed\x95\x9c\xea\xb8\x80\xf0\x9f\x98\x80";
    else if (name == "mixed") unit = "agent:\xed\x95\x9c\xea\xb8\x80\n\"\\value";
    else if (name == "escaped") unit = std::string("\0\n\"\\\t\x1f", 6);
    else throw std::invalid_argument("case must be ascii, unicode, mixed, or escaped");
    std::string value;
    value.reserve(size);
    while (size - value.size() >= unit.size()) value += unit;
    value.append(size - value.size(), 'x');
    return value;
}

std::string scalar_escape(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char byte : value) {
        switch (byte) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (byte < 0x20) {
                    result += "\\u00";
                    result += hex[byte >> 4];
                    result += hex[byte & 15];
                } else result += static_cast<char>(byte);
        }
    }
    return result + '"';
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("usage: bench_canonical_json CASE BYTES ITERATIONS");
        const auto size = positive(argv[2], true), iterations = positive(argv[3]);
        const auto input = payload(argv[1], size);
        const auto expected = scalar_escape(input);
        const neograph::json value = input;
        neograph::json samples = neograph::json::array();
        for (std::size_t i = 0; i < iterations + 20; ++i) {
            const auto begin = std::chrono::steady_clock::now();
            const auto actual = neograph::detail::canonical_json_bytes(value);
            const auto elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - begin).count();
            if (actual != expected) throw std::runtime_error("canonical output differs from scalar oracle");
            if (i >= 20) samples.push_back(elapsed);
        }
        std::cout << neograph::json{{"case", argv[1]}, {"payload_bytes", size},
            {"iterations", iterations}, {"warmup", 20}, {"samples_us", samples},
            {"canonical_output_bytes", expected.size()}, {"status", "ok"},
            {"build_type", NEOGRAPH_BENCH_BUILD_TYPE}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
