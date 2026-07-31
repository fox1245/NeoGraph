/**
 * @file program/pending.h
 * @brief Protocol-neutral durable pending input and effect values.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/json.h>
#include <neograph/program/registry.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace neograph::program {

enum class ProgramPendingInputKind : std::uint8_t {
    Input,
    CapabilityResult,
};

enum class ProgramPendingState : std::uint8_t {
    Awaiting,
    Consumed,
    Expired,
    Cancelled,
    Ambiguous,
};

enum class ProgramEffectIdempotency : std::uint8_t {
    Idempotent,
    NonIdempotent,
};

enum class ProgramEffectReconciliation : std::uint8_t {
    None,
    Completed,
    Failed,
    Unknown,
};

/** Stable result of applying one pending-value transition. */
enum class ProgramPendingDisposition : std::uint8_t {
    Applied,
    Duplicate,
    Conflict,
    WrongPendingId,
    SchemaMismatch,
    Expired,
    Cancelled,
    NotAwaiting,
};

struct ProgramPendingInputData {
    std::string                       operation_id;
    std::string                       call_id;
    ProgramPendingInputKind           kind = ProgramPendingInputKind::Input;
    json                              result_schema = json::object();
    json                              payload       = json::object();
    std::optional<std::uint64_t>      expires_at_unix_ms;
    std::string                       core_node;
    json                              core_interrupt_value;
    ProgramPendingState               state = ProgramPendingState::Awaiting;
    std::optional<json>                consumed_result;

    bool operator==(const ProgramPendingInputData&) const = default;
};

struct ProgramPendingEffectData {
    std::string                       operation_id;
    std::string                       call_id;
    std::string                       effect_id;
    json                              result_schema = json::object();
    json                              payload       = json::object();
    std::optional<std::uint64_t>      expires_at_unix_ms;
    EffectMode                        effect_mode = EffectMode::Brokered;
    ProgramEffectIdempotency          idempotency = ProgramEffectIdempotency::Idempotent;
    std::string                       core_node;
    json                              core_interrupt_value;
    ProgramPendingState               state = ProgramPendingState::Awaiting;
    ProgramEffectReconciliation       reconciliation = ProgramEffectReconciliation::None;
    std::optional<json>                reconciled_result;

    bool operator==(const ProgramPendingEffectData&) const = default;
};

class ProgramPendingInput;
class ProgramPendingEffect;
struct ProgramPendingInputUpdate;
struct ProgramPendingEffectUpdate;

/**
 * Immutable pending host input/capability result.
 *
 * The Core interrupt projection is retained as data, but neither this value nor
 * its transitions know about a transport, server, session, or executor.
 */
class NEOGRAPH_PROGRAM_API ProgramPendingInput {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramPendingInput(ProgramPendingInputData data);
    static ProgramPendingInput parse(std::string_view stored_bytes);

    const std::string&                  operation_id() const noexcept;
    const std::string&                  call_id() const noexcept;
    ProgramPendingInputKind             kind() const noexcept;
    json                                result_schema() const;
    json                                payload() const;
    std::optional<std::uint64_t>        expires_at_unix_ms() const noexcept;
    const std::string&                  core_node() const noexcept;
    json                                core_interrupt_value() const;
    ProgramPendingState                 state() const noexcept;
    std::optional<json>                 consumed_result() const;

    ProgramPendingInputUpdate submit(std::string_view call_id,
                                     const json&      result,
                                     std::uint64_t    now_unix_ms) const;
    ProgramPendingInputUpdate expire(std::uint64_t now_unix_ms) const;
    ProgramPendingInputUpdate cancel() const;

    std::string serialize_canonical() const;
    bool        operator==(const ProgramPendingInput& other) const;

private:
    struct Impl;
    explicit ProgramPendingInput(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramPendingInputUpdate {
    ProgramPendingDisposition disposition;
    ProgramPendingInput       value;
    std::string               diagnostic_code;
    std::string               message;

    bool state_changed() const noexcept;
};

/**
 * Immutable durable external-effect state.
 *
 * A non-idempotent unknown outcome transitions to Ambiguous and cannot become
 * Awaiting again. Only explicit completed/failed reconciliation can consume it;
 * unknown reconciliation deliberately leaves it ambiguous.
 */
class NEOGRAPH_PROGRAM_API ProgramPendingEffect {
public:
    static constexpr std::uint32_t STORAGE_SCHEMA_VERSION = 1;

    explicit ProgramPendingEffect(ProgramPendingEffectData data);
    static ProgramPendingEffect parse(std::string_view stored_bytes);

    const std::string&                  operation_id() const noexcept;
    const std::string&                  call_id() const noexcept;
    const std::string&                  effect_id() const noexcept;
    json                                result_schema() const;
    json                                payload() const;
    std::optional<std::uint64_t>        expires_at_unix_ms() const noexcept;
    EffectMode                          effect_mode() const noexcept;
    ProgramEffectIdempotency            idempotency() const noexcept;
    const std::string&                  core_node() const noexcept;
    json                                core_interrupt_value() const;
    ProgramPendingState                 state() const noexcept;
    ProgramEffectReconciliation         reconciliation() const noexcept;
    std::optional<json>                 reconciled_result() const;
    ProgramPendingEffectUpdate submit(std::string_view call_id,
                                      std::string_view effect_id,
                                      const json&      result,
                                      std::uint64_t    now_unix_ms) const;

    ProgramPendingEffectUpdate mark_outcome_unknown(std::uint64_t now_unix_ms) const;
    ProgramPendingEffectUpdate reconcile(std::string_view                    call_id,
                                         std::string_view                    effect_id,
                                         ProgramEffectReconciliation         resolution,
                                         std::optional<json>                 result,
                                         std::uint64_t                       now_unix_ms) const;
    ProgramPendingEffectUpdate expire(std::uint64_t now_unix_ms) const;
    ProgramPendingEffectUpdate cancel() const;

    std::string serialize_canonical() const;
    bool        operator==(const ProgramPendingEffect& other) const;

private:
    struct Impl;
    explicit ProgramPendingEffect(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

struct ProgramPendingEffectUpdate {
    ProgramPendingDisposition disposition;
    ProgramPendingEffect      value;
    std::string               diagnostic_code;
    std::string               message;

    bool state_changed() const noexcept;
};

NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramPendingInputKind value) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramPendingState value) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramEffectIdempotency value) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramEffectReconciliation value) noexcept;
NEOGRAPH_PROGRAM_API std::string_view to_string(ProgramPendingDisposition value) noexcept;

}  // namespace neograph::program
