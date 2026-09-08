#pragma once

#include <neograph/api.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace neograph::detail {

// Internal selectors support independent correctness checks without mutable
// process-wide overrides. This header is not part of the installed SDK.
enum class Sha256Backend { Automatic, Portable, X86Sha };

constexpr bool sha256_x86_features_supported(std::uint32_t maximum_leaf,
                                             std::uint32_t leaf1_ecx,
                                             std::uint32_t leaf7_ebx) noexcept {
    return maximum_leaf >= 7 && (leaf1_ecx & (1u << 9)) && (leaf1_ecx & (1u << 19)) &&
           (leaf7_ebx & (1u << 29));
}

NEOGRAPH_API bool sha256_hardware_available() noexcept;
NEOGRAPH_API std::array<std::uint8_t, 32> sha256_digest(
    std::string_view input, Sha256Backend backend = Sha256Backend::Automatic);

}  // namespace neograph::detail
