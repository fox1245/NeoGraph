/**
 * @file program/activation.h
 * @brief Owner-scoped activation records for immutable Program versions.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neograph::program {

/** Result of an atomic owner-scoped activation compare-and-swap. */
enum class ProgramActivationResult : std::uint8_t {
    Activated,
    AlreadyPresent,
    Conflict,
};

/**
 * Immutable durable pointer from an owner scope to one admitted version.
 *
 * The generation is monotonically increasing.  Generation zero is the
 * absence of an activation; the first successful activation publishes one.
 */
struct ProgramActivationData {
    std::string   owner_scope;
    std::string   active_version_id;
    std::uint64_t generation = 0;
    std::string   policy_snapshot_hash;
};

class NEOGRAPH_PROGRAM_API ProgramActivation final {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    static ProgramActivation create(ProgramActivationData data);
    static ProgramActivation parse(std::string_view stored_bytes);

    const std::string& owner_scope() const noexcept;
    const std::string& active_version_id() const noexcept;
    std::uint64_t     generation() const noexcept;
    const std::string& policy_snapshot_hash() const noexcept;
    const std::string& id() const noexcept;
    std::string         serialize_canonical() const;

private:
    struct Impl;
    explicit ProgramActivation(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

/** Counts immutable records removed by reference-aware collection. */
struct ProgramRetentionReport {
    std::uint64_t versions_removed = 0;
    std::uint64_t bundles_removed  = 0;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramActivationResult result) noexcept;

}  // namespace neograph::program
