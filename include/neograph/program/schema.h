/**
 * @file program/schema.h
 * @brief Published Program source-schema version identifiers.
 */
#pragma once

#include <cstdint>

namespace neograph::program {
inline constexpr std::uint32_t PROGRAM_SCHEMA_VERSION_V1 = 1;
inline constexpr std::uint32_t PROGRAM_SCHEMA_VERSION_V2 = 2;
inline constexpr std::uint32_t PROGRAM_SCHEMA_VERSION_V3 = 3;
inline constexpr std::uint32_t PROGRAM_SCHEMA_VERSION_V4 = 4;
inline constexpr std::uint32_t LATEST_PROGRAM_SCHEMA_VERSION = PROGRAM_SCHEMA_VERSION_V4;

constexpr bool is_supported_program_schema_version(std::uint32_t version) noexcept {
    return version == PROGRAM_SCHEMA_VERSION_V1 || version == PROGRAM_SCHEMA_VERSION_V2 ||
           version == PROGRAM_SCHEMA_VERSION_V3 || version == PROGRAM_SCHEMA_VERSION_V4;
}

}  // namespace neograph::program
