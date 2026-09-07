#include "sha256.h"

#include <bit>
#include <cstddef>
#include <stdexcept>

#if !defined(NEOGRAPH_DISABLE_SHA256_ACCELERATION) && (defined(__x86_64__) || defined(_M_X64)) && \
    (defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER))
#define NEOGRAPH_SHA256_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

namespace neograph::detail {
namespace {

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

void compress_portable(std::uint32_t*      state,
                       const std::uint8_t* data,
                       std::size_t         blocks) noexcept {
    while (blocks--) {
        const auto*                   block = data;
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
        data += 64;
    }
}

#if defined(NEOGRAPH_SHA256_X86)
// Register layout and SHA-intrinsic flow adapted from Jeffrey Walton's
// public-domain sha256-x86.c, based on Intel / Sean Gulley (miTLS) code.
// https://github.com/noloader/SHA-Intrinsics/blob/d03795497f3e4576083fc2cd8fe0b924f24d0bb2/sha256-x86.c
// The rotating four-vector schedule below retains the same SHA-256 algorithm.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sha,ssse3,sse4.1"), noinline))
#else
__declspec(noinline)
#endif
void compress_x86_sha(std::uint32_t* state, const std::uint8_t* data,
                      std::size_t blocks) noexcept {
    const auto swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    auto       tmp  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state));
    auto       cdgh = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4));
    tmp             = _mm_shuffle_epi32(tmp, 0xb1);
    cdgh            = _mm_shuffle_epi32(cdgh, 0x1b);
    auto abef       = _mm_alignr_epi8(tmp, cdgh, 8);
    cdgh            = _mm_blend_epi16(cdgh, tmp, 0xf0);
    while (blocks--) {
        const auto saved_abef = abef, saved_cdgh = cdgh;
        auto m0 = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data)), swap);
        auto m1 =
            _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16)), swap);
        auto m2 =
            _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 32)), swap);
        auto m3 =
            _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 48)), swap);
        for (std::size_t round = 0; round < 64; round += 4) {
            auto message = _mm_add_epi32(
                m0, _mm_loadu_si128(reinterpret_cast<const __m128i*>(SHA256_K.data() + round)));
            cdgh    = _mm_sha256rnds2_epu32(cdgh, abef, message);
            message = _mm_shuffle_epi32(message, 0x0e);
            abef    = _mm_sha256rnds2_epu32(abef, cdgh, message);
            if (round < 48) {
                auto next = _mm_sha256msg1_epu32(m0, m1);
                next      = _mm_add_epi32(next, _mm_alignr_epi8(m3, m2, 4));
                next      = _mm_sha256msg2_epu32(next, m3);
                m0        = m1;
                m1        = m2;
                m2        = m3;
                m3        = next;
            } else {
                m0 = m1;
                m1 = m2;
                m2 = m3;
            }
        }
        abef = _mm_add_epi32(abef, saved_abef);
        cdgh = _mm_add_epi32(cdgh, saved_cdgh);
        data += 64;
    }
    tmp  = _mm_shuffle_epi32(abef, 0x1b);
    cdgh = _mm_shuffle_epi32(cdgh, 0xb1);
    abef = _mm_blend_epi16(tmp, cdgh, 0xf0);
    cdgh = _mm_alignr_epi8(cdgh, tmp, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), abef);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), cdgh);
}

bool cpu_has_sha() noexcept {
    std::uint32_t maximum, leaf1, leaf7;
#if defined(_MSC_VER)
    int registers[4];
    __cpuidex(registers, 0, 0);
    maximum = static_cast<std::uint32_t>(registers[0]);
    if (maximum < 7) return false;
    __cpuidex(registers, 1, 0);
    leaf1 = static_cast<std::uint32_t>(registers[2]);
    __cpuidex(registers, 7, 0);
    leaf7 = static_cast<std::uint32_t>(registers[1]);
#else
    maximum = __get_cpuid_max(0, nullptr);
    if (maximum < 7) return false;
    unsigned int a, b, c, d;
    __cpuid_count(1, 0, a, b, c, d);
    leaf1 = c;
    __cpuid_count(7, 0, a, b, c, d);
    leaf7 = b;
#endif
    return sha256_x86_features_supported(maximum, leaf1, leaf7);
}
#endif

using Compressor = void (*)(std::uint32_t*, const std::uint8_t*, std::size_t) noexcept;
Compressor automatic_compressor() noexcept {
#if defined(NEOGRAPH_SHA256_X86)
    if (sha256_hardware_available()) return compress_x86_sha;
#endif
    return compress_portable;
}

}  // namespace

bool sha256_hardware_available() noexcept {
#if defined(NEOGRAPH_SHA256_X86)
    static const bool available = cpu_has_sha();
    return available;
#else
    return false;
#endif
}

std::array<std::uint8_t, 32> sha256_digest(std::string_view input, Sha256Backend backend) {
    static const Compressor automatic = automatic_compressor();
    Compressor              compress;
    switch (backend) {
        case Sha256Backend::Automatic:
            compress = automatic;
            break;
        case Sha256Backend::Portable:
            compress = compress_portable;
            break;
        case Sha256Backend::X86Sha:
#if defined(NEOGRAPH_SHA256_X86)
            if (sha256_hardware_available()) {
                compress = compress_x86_sha;
                break;
            }
#endif
            throw std::invalid_argument("SHA-256 hardware backend is unavailable");
        default:
            throw std::invalid_argument("Unknown SHA-256 backend");
    }
    std::array<std::uint32_t, 8> state = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    const auto full_blocks = input.size() / 64;
    if (full_blocks)
        compress(state.data(), reinterpret_cast<const std::uint8_t*>(input.data()), full_blocks);
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
    compress(state.data(), tail.data(), tail_size / 64);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < state.size(); ++i) {
        digest[i * 4]     = static_cast<std::uint8_t>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

}  // namespace neograph::detail
