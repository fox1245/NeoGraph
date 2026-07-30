#include "canonical_json.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neograph::program::detail {
namespace {

constexpr std::size_t MAX_JSON_BYTES = 16u * 1024u * 1024u;
constexpr std::size_t MAX_JSON_DEPTH = 256;

void validate_json_text_limits(std::string_view bytes) {
    if (bytes.size() > MAX_JSON_BYTES) {
        throw std::invalid_argument("Program JSON exceeds the 16 MiB input limit");
    }
    std::size_t depth     = 0;
    bool        in_string = false;
    bool        escaped   = false;
    for (const char value : bytes) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            if (++depth > MAX_JSON_DEPTH) {
                throw std::invalid_argument("Program JSON exceeds the maximum nesting depth");
            }
        } else if ((value == '}' || value == ']') && depth != 0) {
            --depth;
        }
    }
}

void validate_strict_tree(const json& value) {
    if (value.is_object()) {
        std::set<std::string> fields;
        for (const auto& [field, child] : value.items()) {
            if (!fields.insert(field).second) {
                throw std::invalid_argument("Program JSON contains duplicate object field '" +
                                            field + "'");
            }
            validate_strict_tree(child);
        }
    } else if (value.is_array()) {
        for (const auto& child : value)
            validate_strict_tree(child);
    }
}

std::size_t checked_json_size(std::size_t current, std::size_t additional) {
    if (additional > MAX_JSON_BYTES - current) {
        throw std::invalid_argument("Program JSON exceeds the 16 MiB materialized limit");
    }
    return current + additional;
}

std::size_t escaped_json_string_size(std::string_view value) {
    std::size_t size = 2;
    for (const unsigned char byte : value) {
        size = checked_json_size(size, byte < 0x20 ? 6 : ((byte == '"' || byte == '\\') ? 2 : 1));
    }
    return size;
}

std::size_t validate_json_value_limits(const json& value, std::size_t depth = 1) {
    if (depth > MAX_JSON_DEPTH) {
        throw std::invalid_argument("Program JSON exceeds the maximum nesting depth");
    }
    if (value.is_null()) return 4;
    if (value.is_boolean()) return value.get<bool>() ? 4 : 5;
    if (value.is_number()) return 32;
    if (value.is_string()) return escaped_json_string_size(value.get<std::string>());

    std::size_t size  = 2;
    bool        first = true;
    if (value.is_array()) {
        for (const auto& child : value) {
            if (!first) size = checked_json_size(size, 1);
            first = false;
            size  = checked_json_size(size, validate_json_value_limits(child, depth + 1));
        }
        return size;
    }
    if (value.is_object()) {
        for (const auto& [field, child] : value.items()) {
            if (!first) size = checked_json_size(size, 1);
            first = false;
            size  = checked_json_size(size, escaped_json_string_size(field));
            size  = checked_json_size(size, 1);
            size  = checked_json_size(size, validate_json_value_limits(child, depth + 1));
        }
        return size;
    }
    throw std::invalid_argument("Program JSON contains an unsupported value");
}

json owned_json_copy_impl(const json& value, std::size_t depth) {
    if (depth > MAX_JSON_DEPTH) {
        throw std::invalid_argument("Program JSON exceeds the maximum nesting depth");
    }
    if (value.is_null()) return nullptr;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) return value.get<unsigned long long>();
    if (value.is_number_integer()) return value.get<long long>();
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) {
        json copy = json::array();
        for (const auto& item : value)
            copy.push_back(owned_json_copy_impl(item, depth + 1));
        return copy;
    }
    if (value.is_object()) {
        json copy = json::object();
        for (const auto& [key, item] : value.items())
            copy[key] = owned_json_copy_impl(item, depth + 1);
        return copy;
    }
    throw std::invalid_argument("Program JSON contains an unsupported value");
}

constexpr bool ascii_digit(unsigned char value) noexcept {
    return value >= '0' && value <= '9';
}

constexpr bool ascii_alnum(unsigned char value) noexcept {
    return ascii_digit(value) || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

bool valid_numeric_identifier(std::string_view value) {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return false;
    return std::all_of(value.begin(), value.end(), ascii_digit);
}

bool valid_identifier(std::string_view value, bool prerelease) {
    if (value.empty()) return false;
    const bool numeric = std::all_of(value.begin(), value.end(), ascii_digit);
    if (prerelease && numeric && value.size() > 1 && value.front() == '0') return false;
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return ascii_alnum(c) || c == '-'; });
}

bool valid_dot_identifiers(std::string_view value, bool prerelease) {
    if (value.empty()) return false;
    while (true) {
        const auto dot = value.find('.');
        if (!valid_identifier(value.substr(0, dot), prerelease)) return false;
        if (dot == std::string_view::npos) return true;
        value.remove_prefix(dot + 1);
    }
}

std::size_t validated_utf8_sequence_length(std::string_view value, std::size_t index) {
    const auto byte_at = [&value](std::size_t offset) {
        return static_cast<unsigned char>(value[offset]);
    };
    const auto require_continuation = [&byte_at, &value](std::size_t offset) {
        if (offset >= value.size() || (byte_at(offset) & 0xc0u) != 0x80u) {
            throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
        }
    };

    const auto lead = byte_at(index);
    if (lead < 0x80) return 1;
    if (lead >= 0xc2 && lead <= 0xdf) {
        require_continuation(index + 1);
        return 2;
    }
    if (lead == 0xe0) {
        if (index + 1 >= value.size() || byte_at(index + 1) < 0xa0 || byte_at(index + 1) > 0xbf) {
            throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
        }
        require_continuation(index + 2);
        return 3;
    }
    if ((lead >= 0xe1 && lead <= 0xec) || (lead >= 0xee && lead <= 0xef)) {
        require_continuation(index + 1);
        require_continuation(index + 2);
        return 3;
    }
    if (lead == 0xed) {
        if (index + 1 >= value.size() || byte_at(index + 1) < 0x80 || byte_at(index + 1) > 0x9f) {
            throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
        }
        require_continuation(index + 2);
        return 3;
    }
    if (lead == 0xf0) {
        if (index + 1 >= value.size() || byte_at(index + 1) < 0x90 || byte_at(index + 1) > 0xbf) {
            throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
        }
        require_continuation(index + 2);
        require_continuation(index + 3);
        return 4;
    }
    if (lead >= 0xf1 && lead <= 0xf3) {
        require_continuation(index + 1);
        require_continuation(index + 2);
        require_continuation(index + 3);
        return 4;
    }
    if (lead == 0xf4) {
        if (index + 1 >= value.size() || byte_at(index + 1) < 0x80 || byte_at(index + 1) > 0x8f) {
            throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
        }
        require_continuation(index + 2);
        require_continuation(index + 3);
        return 4;
    }
    throw std::invalid_argument("Program canonical JSON rejects invalid UTF-8");
}

void validate_utf8_impl(std::string_view value) {
    for (std::size_t index = 0; index < value.size();) {
        index += validated_utf8_sequence_length(value, index);
    }
}

void append_escaped(std::string& out, std::string_view value) {
    validate_utf8_impl(value);
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 0x0f]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void append_float(std::string& out, double value) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument("Program canonical JSON rejects non-finite numbers");
    }
    if (value == 0.0) {
        out.push_back('0');
        return;
    }

    std::array<char, 64> buffer{};
    const auto           result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{}) {
        throw std::runtime_error("Program canonical JSON failed to encode number");
    }
    std::string encoded(buffer.data(), result.ptr);
    const auto  exponent = encoded.find_first_of("eE");
    if (exponent != std::string::npos) {
        encoded[exponent]  = 'e';
        std::size_t digits = exponent + 1;
        if (digits < encoded.size() && encoded[digits] == '+') {
            encoded.erase(digits, 1);
        } else if (digits < encoded.size() && encoded[digits] == '-') {
            ++digits;
        }
        while (digits + 1 < encoded.size() && encoded[digits] == '0') {
            encoded.erase(digits, 1);
        }
    }
    out += encoded;
}

void append_value(std::string& out, const json& value) {
    if (value.is_null()) {
        out += "null";
    } else if (value.is_boolean()) {
        out += value.get<bool>() ? "true" : "false";
    } else if (value.is_number_unsigned()) {
        out += std::to_string(value.get<unsigned long long>());
    } else if (value.is_number_integer()) {
        out += std::to_string(value.get<long long>());
    } else if (value.is_number_float()) {
        append_float(out, value.get<double>());
    } else if (value.is_string()) {
        append_escaped(out, value.get<std::string>());
    } else if (value.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto element : value) {
            if (!first) out.push_back(',');
            first = false;
            append_value(out, element);
        }
        out.push_back(']');
    } else if (value.is_object()) {
        std::vector<std::pair<std::string, json>> fields;
        fields.reserve(value.size());
        for (const auto& [key, child] : value.items()) {
            fields.emplace_back(key, child);
        }
        std::sort(fields.begin(), fields.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        if (std::adjacent_find(fields.begin(), fields.end(), [](const auto& lhs, const auto& rhs) {
                return lhs.first == rhs.first;
            }) != fields.end()) {
            throw std::invalid_argument(
                "Program canonical JSON rejects duplicate object member names");
        }

        out.push_back('{');
        bool first = true;
        for (const auto& [key, child] : fields) {
            if (!first) out.push_back(',');
            first = false;
            append_escaped(out, key);
            out.push_back(':');
            append_value(out, child);
        }
        out.push_back('}');
    } else {
        throw std::invalid_argument("Program canonical JSON encountered unsupported value");
    }
}

constexpr std::array<std::uint32_t, 64> SHA256_K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

std::array<std::uint8_t, 32> sha256(std::string_view input) {
    std::array<std::uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    const auto compress = [&state](const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto base = i * 4;
            words[i]        = (static_cast<std::uint32_t>(block[base]) << 24) |
                       (static_cast<std::uint32_t>(block[base + 1]) << 16) |
                       (static_cast<std::uint32_t>(block[base + 2]) << 8) |
                       static_cast<std::uint32_t>(block[base + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 =
                std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 =
                std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        auto a = state[0];
        auto b = state[1];
        auto c = state[2];
        auto d = state[3];
        auto e = state[4];
        auto f = state[5];
        auto g = state[6];
        auto h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1       = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choose   = (e & f) ^ (~e & g);
            const auto t1       = h + s1 + choose + SHA256_K[i] + words[i];
            const auto s0       = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2       = s0 + majority;
            h                   = g;
            g                   = f;
            f                   = e;
            e                   = d + t1;
            d                   = c;
            c                   = b;
            b                   = a;
            a                   = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    const auto full_blocks = input.size() / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        const auto* block = reinterpret_cast<const std::uint8_t*>(input.data() + i * 64);
        compress(block);
    }

    std::array<std::uint8_t, 128> tail{};
    const auto                    remainder = input.size() % 64;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] =
            static_cast<std::uint8_t>(static_cast<unsigned char>(input[full_blocks * 64 + i]));
    }
    tail[remainder]              = 0x80;
    const std::size_t tail_size  = remainder < 56 ? 64 : 128;
    const auto        bit_length = static_cast<std::uint64_t>(input.size()) * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_size - 1 - i] = static_cast<std::uint8_t>(bit_length >> (i * 8));
    }
    compress(tail.data());
    if (tail_size == 128) {
        compress(tail.data() + 64);
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        digest[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

}  // namespace

bool is_semantic_version(std::string_view value) noexcept {
    const auto plus = value.find('+');
    if (plus != std::string_view::npos) {
        if (!valid_dot_identifiers(value.substr(plus + 1), false)) return false;
        value = value.substr(0, plus);
    }
    const auto dash = value.find('-');
    if (dash != std::string_view::npos) {
        if (!valid_dot_identifiers(value.substr(dash + 1), true)) return false;
        value = value.substr(0, dash);
    }
    const auto first = value.find('.');
    const auto second =
        first == std::string_view::npos ? std::string_view::npos : value.find('.', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        value.find('.', second + 1) != std::string_view::npos) {
        return false;
    }
    return valid_numeric_identifier(value.substr(0, first)) &&
           valid_numeric_identifier(value.substr(first + 1, second - first - 1)) &&
           valid_numeric_identifier(value.substr(second + 1));
}

json owned_json_copy(const json& value) {
    (void)validate_json_value_limits(value);
    validate_strict_tree(value);
    return owned_json_copy_impl(value, 1);
}

json parse_json_strict(std::string_view bytes) {
    validate_json_text_limits(bytes);
    auto value = json::parse(bytes);
    validate_strict_tree(value);
    (void)canonical_json_bytes(value);
    return value;
}

std::string canonical_json_bytes(const json& value) {
    (void)validate_json_value_limits(value);
    std::string result;
    append_value(result, value);
    return result;
}
void validate_utf8(std::string_view value) {
    validate_utf8_impl(value);
}
void validate_token(std::string_view value, std::string_view field) {
    if (value.empty()) {
        throw std::invalid_argument(std::string(field) + " must not be empty");
    }
    validate_utf8_impl(value);
    const auto invalid_control = [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; };
    if (std::any_of(value.begin(), value.end(), invalid_control)) {
        throw std::invalid_argument(std::string(field) + " must not contain control characters");
    }
    if (value.front() == ' ' || value.back() == ' ') {
        throw std::invalid_argument(std::string(field) +
                                    " must not have leading or trailing whitespace");
    }
}
void validate_json_pointer(std::string_view value) {
    validate_utf8_impl(value);
    if (value.empty()) return;
    if (value.front() != '/') {
        throw std::invalid_argument("JSON Pointer must be empty or start with '/'");
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '~') continue;
        if (++index == value.size() || (value[index] != '0' && value[index] != '1')) {
            throw std::invalid_argument("JSON Pointer contains an invalid escape");
        }
    }
}
void reject_unknown_fields(const json&                             value,
                           std::string_view                        object_name,
                           std::initializer_list<std::string_view> allowed_fields) {
    if (!value.is_object()) {
        throw std::invalid_argument(std::string(object_name) + " must be an object");
    }
    for (auto field = value.begin(); field != value.end(); ++field) {
        const auto key = field.key();
        const auto known =
            std::find(allowed_fields.begin(), allowed_fields.end(), std::string_view(key));
        if (known == allowed_fields.end()) {
            throw std::invalid_argument(std::string(object_name) + " contains undeclared field '" +
                                        key + "'");
        }
    }
}

std::string sha256_identity(std::string_view domain, std::string_view bytes) {
    std::string input = "NeoGraph Program identity v1";
    input.push_back('\0');
    input.append(domain);
    input.push_back('\0');
    input += std::to_string(bytes.size());
    input.push_back('\0');
    input.append(bytes);

    static constexpr char hex[]  = "0123456789abcdef";
    const auto            digest = sha256(input);
    std::string           result = "sha256:";
    result.reserve(71);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool is_sha256_identity(std::string_view value) noexcept {
    if (value.size() != 71 || !value.starts_with("sha256:")) return false;
    return std::all_of(value.begin() + 7, value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

}  // namespace neograph::program::detail
