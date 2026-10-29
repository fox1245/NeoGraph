#include "../src/core/sha256.h"
#include "../src/core/canonical_json.h"
#include "sha256_test_vectors.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace {
using neograph::detail::Sha256Backend;
using neograph::detail::Sha256Stream;
using neograph::detail::sha256_digest;

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (const auto value : digest) {
        result += alphabet[value >> 4];
        result += alphabet[value & 15];
    }
    return result;
}

std::string ramp(std::size_t size) {
    std::string result(size, '\0');
    for (std::size_t i = 0; i < size; ++i) result[i] = static_cast<char>(i % 251);
    return result;
}

// Every hardware backend this CPU and build provide. Empty when only the portable code is available.
std::vector<Sha256Backend> hardware_backends() {
    std::vector<Sha256Backend> result;
    for (const auto backend : {Sha256Backend::X86Sha, Sha256Backend::Arm64Sha})
        if (neograph::detail::sha256_backend_available(backend)) result.push_back(backend);
    return result;
}

std::vector<Sha256Backend> backends() {
    std::vector<Sha256Backend> result{Sha256Backend::Portable, Sha256Backend::Automatic};
    for (const auto backend : hardware_backends()) result.push_back(backend);
    return result;
}

// xorshift32 stream with a fixed seed: the same bytes and chunk sizes on every platform.
class Xorshift {
public:
    explicit Xorshift(std::uint32_t seed) : state_(seed) {}
    std::uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

private:
    std::uint32_t state_;
};
}

TEST(Sha256Test, StandardKnownAnswersMatchEveryAvailableBackend) {
    const std::vector<std::pair<std::string, std::string>> vectors{
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for (const auto backend : backends()) {
        for (const auto& [input, expected] : vectors)
            EXPECT_EQ(hex(sha256_digest(input, backend)), expected) << input.size();
        EXPECT_EQ(hex(sha256_digest(std::string_view{}, backend)), vectors.front().second);
    }
}

TEST(Sha256Test, IndependentBinaryVectorsCoverPaddingAndUnalignedInputs) {
    for (const auto& vector : SHA256_BINARY_VECTORS) {
        const auto input = ramp(vector.size);
        for (const auto backend : backends()) {
            for (std::size_t offset = 0; offset < (vector.size < 256 ? 16U : 1U); ++offset) {
                const auto storage = std::string(offset, 'x') + input;
                const auto view = std::string_view(storage).substr(offset);
                EXPECT_EQ(hex(sha256_digest(view, backend)), vector.raw)
                    << "size=" << vector.size << " offset=" << offset;
            }
        }
    }
}

TEST(Sha256Test, AStreamGivesTheDigestOfItsPiecesWhateverTheirSize) {
    for (const auto& vector : SHA256_BINARY_VECTORS) {
        const auto input = ramp(vector.size);
        for (const auto backend : backends()) {
            for (const std::size_t piece : {1U, 7U, 63U, 64U, 65U, 1000U}) {
                Sha256Stream hash(backend);
                for (std::size_t at = 0; at < input.size(); at += piece)
                    hash.update(std::string_view(input).substr(at, piece));
                hash.update(std::string_view{});
                EXPECT_EQ(hex(hash.finish()), vector.raw) << "size=" << vector.size << " piece=" << piece;
            }
        }
    }
    Sha256Stream empty;
    EXPECT_EQ(hex(empty.finish()), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, AFinishedStreamTakesNoMoreInput) {
    Sha256Stream hash;
    hash.update("abc");
    EXPECT_EQ(hex(hash.finish()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_THROW(hash.update("d"), std::logic_error);
    EXPECT_THROW((void)hash.finish(), std::logic_error);
    EXPECT_THROW((void)Sha256Stream(static_cast<Sha256Backend>(99)), std::invalid_argument);
}

TEST(Sha256Test, IdentityFramingMatchesIndependentKnownBytes) {
    for (const auto& vector : SHA256_BINARY_VECTORS) {
        const auto input = ramp(vector.size);
        EXPECT_EQ(neograph::detail::sha256_identity("sha-benchmark/v1", "record", input),
                  std::string("sha256:") + std::string(vector.identity));
    }
    const std::string preamble("pre\0amble", 9), domain("d\0omain", 7), bytes("\0\xffx", 3);
    EXPECT_EQ(neograph::detail::sha256_identity(preamble, domain, bytes), SHA256_NUL_IDENTITY);
}

TEST(Sha256Test, CpuDispatchRequiresEveryInstructionSetUsed) {
    constexpr std::uint32_t ssse3 = 1u << 9, sse41 = 1u << 19, sha = 1u << 29;
    using neograph::detail::sha256_x86_features_supported;
    EXPECT_FALSE(sha256_x86_features_supported(6, ssse3 | sse41, sha));
    EXPECT_FALSE(sha256_x86_features_supported(7, sse41, sha));
    EXPECT_FALSE(sha256_x86_features_supported(7, ssse3, sha));
    EXPECT_FALSE(sha256_x86_features_supported(7, ssse3 | sse41, 0));
    EXPECT_TRUE(sha256_x86_features_supported(7, ssse3 | sse41, sha));
    EXPECT_THROW((void)sha256_digest("abc", static_cast<Sha256Backend>(99)), std::invalid_argument);
}

TEST(Sha256Test, ABackendThatIsNotAvailableIsRefusedAndAnAvailableOneIsNot) {
    using neograph::detail::sha256_backend_available;
    EXPECT_TRUE(sha256_backend_available(Sha256Backend::Automatic));
    EXPECT_TRUE(sha256_backend_available(Sha256Backend::Portable));
    EXPECT_FALSE(sha256_backend_available(static_cast<Sha256Backend>(99)));
    const bool x86 = sha256_backend_available(Sha256Backend::X86Sha);
    const bool arm = sha256_backend_available(Sha256Backend::Arm64Sha);
    EXPECT_FALSE(x86 && arm) << "one CPU architecture offers one hardware backend";
    EXPECT_EQ(neograph::detail::sha256_hardware_available(), x86 || arm);
    for (const auto backend : {Sha256Backend::X86Sha, Sha256Backend::Arm64Sha}) {
        if (sha256_backend_available(backend)) {
            EXPECT_NO_THROW((void)Sha256Stream(backend));
        } else {
            EXPECT_THROW((void)sha256_digest("abc", backend), std::invalid_argument);
            EXPECT_THROW((void)Sha256Stream(backend), std::invalid_argument);
        }
    }
}

TEST(Sha256Test, HardwareAndPortableAgreeAcrossDeterministicCorpus) {
    std::uint32_t state = 0x8fac013u;
    for (std::size_t sample = 0; sample < 512; ++sample) {
        std::string input(sample * 13 % 4097, '\0');
        for (char& byte : input) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            byte = static_cast<char>(state);
        }
        const auto portable = sha256_digest(input, Sha256Backend::Portable);
        EXPECT_EQ(sha256_digest(input), portable);
        for (const auto backend : hardware_backends())
            EXPECT_EQ(sha256_digest(input, backend), portable);
    }
}

TEST(Sha256Test, EveryBackendMatchesPortableForEveryLengthUpToAKilobyte) {
    Xorshift random(0x5eed1234u);
    std::string input(1024, '\0');
    for (char& byte : input) byte = static_cast<char>(random.next() >> 11);
    for (std::size_t length = 0; length <= input.size(); ++length) {
        const auto view     = std::string_view(input).substr(0, length);
        const auto portable = sha256_digest(view, Sha256Backend::Portable);
        for (const auto backend : backends())
            ASSERT_EQ(hex(sha256_digest(view, backend)), hex(portable))
                << "length=" << length << " backend=" << static_cast<int>(backend);
    }
}

TEST(Sha256Test, EveryBackendMatchesPortableOnLargeBuffersFedInRandomPieces) {
    Xorshift random(0xc0ffee11u);
    // Sizes around block and megabyte boundaries, so the tail padding takes both the one-block and
    // two-block form.
    for (const std::size_t size : {1048576u + 55u, 1048576u + 56u, 3u * 1048576u + 64u + 17u}) {
        std::string input(size, '\0');
        for (char& byte : input) byte = static_cast<char>(random.next() >> 9);
        const auto portable = hex(sha256_digest(input, Sha256Backend::Portable));
        for (const auto backend : backends()) {
            Sha256Stream hash(backend);
            std::size_t  at = 0;
            while (at < input.size()) {
                // Mostly small pieces (every residue mod 64), sometimes a multi-block run.
                const std::size_t piece =
                    random.next() % 8 == 0 ? random.next() % 70000u : random.next() % 200u;
                hash.update(std::string_view(input).substr(at, piece));
                at += std::min(piece, input.size() - at);
            }
            EXPECT_EQ(hex(hash.finish()), portable)
                << "size=" << size << " backend=" << static_cast<int>(backend);
        }
    }
}

TEST(Sha256Test, IndependentConcurrentCallsHaveNoSharedDigestState) {
    std::vector<std::future<bool>> workers;
    for (std::size_t worker = 0; worker < 8; ++worker) {
        workers.push_back(std::async(std::launch::async, [worker] {
            for (std::size_t i = 0; i < 128; ++i) {
                const auto input = ramp(i * 13 + worker);
                if (sha256_digest(input) != sha256_digest(input, Sha256Backend::Portable)) return false;
            }
            return true;
        }));
    }
    for (auto& worker : workers) EXPECT_TRUE(worker.get());
}
