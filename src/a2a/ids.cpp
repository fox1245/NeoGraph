#include "ids.h"

#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace neograph::a2a::detail {

std::string random_uuid() {
    std::array<unsigned char, 16> bytes{};
    if (::RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("A2A: the cryptographic random generator failed");
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // RFC 9562 variant
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

namespace {

// Days from 1970-01-01 to the civil date (proleptic Gregorian), after
// Howard Hinnant's days_from_civil.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9;
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool read_digits(std::string_view text, std::size_t& pos, std::size_t count, int& value) {
    if (pos + count > text.size()) return false;
    value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[pos + i];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    pos += count;
    return true;
}

}  // namespace

std::string utc_timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    // Room for any int fields, so the compiler can prove no truncation.
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                  utc.tm_min, utc.tm_sec, static_cast<int>(ms % 1000));
    return buf;
}

std::optional<std::int64_t> parse_timestamp_ms(std::string_view text) {
    std::size_t pos = 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!read_digits(text, pos, 4, year) || pos >= text.size() || text[pos++] != '-' ||
        !read_digits(text, pos, 2, month) || pos >= text.size() || text[pos++] != '-' ||
        !read_digits(text, pos, 2, day) || pos >= text.size() ||
        (text[pos] != 'T' && text[pos] != 't')) {
        return std::nullopt;
    }
    ++pos;
    if (!read_digits(text, pos, 2, hour) || pos >= text.size() || text[pos++] != ':' ||
        !read_digits(text, pos, 2, minute) || pos >= text.size() || text[pos++] != ':' ||
        !read_digits(text, pos, 2, second)) {
        return std::nullopt;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {
        return std::nullopt;
    }
    int millis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) millis = millis * 10 + (text[pos] - '0');
            ++digits;
            ++pos;
        }
        if (digits == 0) return std::nullopt;
        for (int i = digits; i < 3; ++i) millis *= 10;
    }
    if (pos >= text.size()) return std::nullopt;
    std::int64_t offset_minutes = 0;
    if (text[pos] == 'Z' || text[pos] == 'z') {
        ++pos;
    } else if (text[pos] == '+' || text[pos] == '-') {
        const int sign = text[pos] == '-' ? -1 : 1;
        ++pos;
        int oh = 0, om = 0;
        if (!read_digits(text, pos, 2, oh) || pos >= text.size() || text[pos++] != ':' ||
            !read_digits(text, pos, 2, om) || oh > 23 || om > 59) {
            return std::nullopt;
        }
        offset_minutes = sign * (std::int64_t{oh} * 60 + om);
    } else {
        return std::nullopt;
    }
    if (pos != text.size()) return std::nullopt;
    const auto days = days_from_civil(year, static_cast<unsigned>(month),
                                      static_cast<unsigned>(day));
    // Every term is computed in int64_t. The fields are range-checked above, so no int product
    // could overflow, but the arithmetic does not rely on that.
    const std::int64_t secs = days * 86400 + std::int64_t{hour} * 3600 + std::int64_t{minute} * 60 +
                              second - offset_minutes * 60;
    return secs * 1000 + millis;
}

}  // namespace neograph::a2a::detail
