#pragma once

#include <neograph/api.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace neograph::detail {

// Internal selectors support independent correctness checks without mutable
// process-wide overrides. This header is not part of the installed SDK.
enum class Sha256Backend { Automatic, Portable, X86Sha, Arm64Sha };

constexpr bool sha256_x86_features_supported(std::uint32_t maximum_leaf,
                                             std::uint32_t leaf1_ecx,
                                             std::uint32_t leaf7_ebx) noexcept {
    return maximum_leaf >= 7 && (leaf1_ecx & (1u << 9)) && (leaf1_ecx & (1u << 19)) &&
           (leaf7_ebx & (1u << 29));
}

// True when the CPU running this process has the instructions the given backend uses and this build
// compiled it in. Automatic and Portable are always available; a backend that is not available
// makes sha256_digest and Sha256Stream throw std::invalid_argument.
NEOGRAPH_API bool sha256_backend_available(Sha256Backend backend) noexcept;
// True when any hardware backend (X86Sha or Arm64Sha) is available; Automatic then uses it.
NEOGRAPH_API bool sha256_hardware_available() noexcept;
NEOGRAPH_API std::array<std::uint8_t, 32> sha256_digest(
    std::string_view input, Sha256Backend backend = Sha256Backend::Automatic);

// SHA-256 of input given in pieces of any size, so a large file is hashed without being held in memory.
// finish() returns the digest of everything given to update(); a finished hash takes no more input.
class NEOGRAPH_API Sha256Stream {
public:
    explicit Sha256Stream(Sha256Backend backend = Sha256Backend::Automatic);
    void update(std::string_view input);
    std::array<std::uint8_t, 32> finish();

private:
    using Compressor = void (*)(std::uint32_t*, const std::uint8_t*, std::size_t) noexcept;
    Compressor                   compress_;
    std::array<std::uint32_t, 8> state_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t                  filled_   = 0;
    std::uint64_t                length_   = 0;
    bool                         finished_ = false;
};

}  // namespace neograph::detail
