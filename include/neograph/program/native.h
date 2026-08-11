/**
 * @file program/native.h
 * @brief C++ ownership-safe wrapper for the Program native C v1 ABI.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/program/bundle.h>
#include <neograph/program/native_abi.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace neograph::program {
/**
 * Import slots below this value are reserved by the versioned `ng` command
 * module. Native controls must be explicitly sealed at or above this boundary.
 */
inline constexpr std::uint32_t NATIVE_CONTROL_IMPORT_SLOT_MIN = 7;

enum class NativeIdempotency : std::uint8_t {
    Idempotent,
    NonIdempotent,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(NativeIdempotency value) noexcept;
NEOGRAPH_PROGRAM_API NativeIdempotency native_idempotency_from_string(std::string_view value);

enum class NativeReplayBehavior : std::uint8_t {
    Deterministic,
    Recorded,
    Unmanaged,
};

NEOGRAPH_PROGRAM_API std::string_view to_string(NativeReplayBehavior value) noexcept;
NEOGRAPH_PROGRAM_API NativeReplayBehavior
native_replay_behavior_from_string(std::string_view value);

/**
 * Declared native resource envelope.
 *
 * `max_input_bytes` and `max_output_bytes` are enforced by the host wrapper.
 * `advisory_wall_time_ms` and `advisory_memory_bytes` describe expected trusted
 * callback demand only; they are never presented as process containment.
 * ProgramRuntime's separately granted run deadline starts before the
 * synchronous callback is entered and rejects a late result, but cannot
 * preempt the callback or contain arbitrary plugin allocation.
 *
 * Consequently, ProgramCatalog materializes NativeControlBinding only for a
 * TrustedNative executable admitted through TrustedEmbedding. Multi-tenant
 * Brokered executables require a separately contained, killable broker process
 * which this ABI does not provide.
 */
struct NativeResourceDeclaration {
    std::size_t   max_input_bytes       = 0;
    std::size_t   max_output_bytes      = 0;
    std::uint64_t advisory_wall_time_ms = 0;
    std::size_t   advisory_memory_bytes = 0;

    bool operator==(const NativeResourceDeclaration&) const = default;
};

/** Immutable metadata which participates in the registry snapshot identity. */
struct NativeControlMetadata {
    ContractRecord       input_contract;
    ContractRecord       output_contract;
    NativeIdempotency    idempotency     = NativeIdempotency::NonIdempotent;
    NativeReplayBehavior replay_behavior = NativeReplayBehavior::Recorded;
    NativeResourceDeclaration resource_declaration;
};

enum class NativeInvocationStatus : std::uint8_t {
    Success,
    Failure,
    Cancelled,
    ProtocolFailure,
};

/** Host-owned terminal result; no plugin storage survives this value. */
struct NativeInvocationResult {
    NativeInvocationStatus status = NativeInvocationStatus::ProtocolFailure;
    json                   value  = nullptr;
    std::string            diagnostic_code;
    std::string            diagnostic_message;
};

namespace detail {
struct NativeControlBindingImpl;
struct NativeInvocationImpl;
}  // namespace detail

/**
 * One move-only handle for an accepted native invocation.
 *
 * The C callback may complete after the originating NativeControlBinding value
 * is destroyed. The plugin's destroy callback therefore runs only after this
 * handle and the callback lease have both gone away.
 */
class NEOGRAPH_PROGRAM_API NativeInvocation final {
public:
    NativeInvocation() = default;
    NativeInvocation(NativeInvocation&&) noexcept;
    NativeInvocation& operator=(NativeInvocation&&) noexcept;
    NativeInvocation(const NativeInvocation&)            = delete;
    NativeInvocation& operator=(const NativeInvocation&) = delete;
    ~NativeInvocation();
    void cancel();
    bool cancel_requested() const noexcept;
    bool finished() const noexcept;
    bool wait_for(std::chrono::milliseconds timeout) const;
    std::optional<NativeInvocationResult> result() const;

private:
    explicit NativeInvocation(std::shared_ptr<detail::NativeInvocationImpl> impl);

    std::shared_ptr<detail::NativeInvocationImpl> impl_;
    friend class NativeControlBinding;
};

/**
 * C++ wrapper over a C v1 plugin binding for trusted in-process dispatch only.
 * The invoke callback is entered synchronously, though it may arrange an
 * asynchronous completion. The wrapper accepts canonical JSON inputs, copies
 * completion bytes before invoking the plugin release hook, and never allows
 * an exception from a non-conforming plugin to escape the host.
 *
 * This type is not a broker or memory sandbox. Catalog admission therefore
 * seals it only to a TrustedNative executable in a TrustedEmbedding profile.
 */
class NEOGRAPH_PROGRAM_API NativeControlBinding final {
public:
    NativeControlBinding() = default;
    NativeControlBinding(const NativeControlBinding&) noexcept;
    NativeControlBinding(NativeControlBinding&&) noexcept;
    NativeControlBinding& operator=(const NativeControlBinding&) noexcept;
    NativeControlBinding& operator=(NativeControlBinding&&) noexcept;
    ~NativeControlBinding();

    static NativeControlBinding create(neograph_program_native_binding_v1 binding,
                                       NativeControlMetadata              metadata);

    bool                         valid() const noexcept;
    const NativeControlMetadata& metadata() const;
    NativeInvocation             invoke(std::uint64_t invocation_id, const json& input) const;

private:
    explicit NativeControlBinding(std::shared_ptr<detail::NativeControlBindingImpl> impl);

    std::shared_ptr<detail::NativeControlBindingImpl> impl_;
};

}  // namespace neograph::program
