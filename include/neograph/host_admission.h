/**
 * @file host_admission.h
 * @brief Bounded host-resource admission for Program and external work.
 *
 * Host admission is intentionally separate from Program's durable budget ledger:
 * a Program budget says what one request is allowed to spend, while this API
 * bounds what the local host may reserve concurrently.  A zero capacity is a
 * real denial, never an "unknown means unlimited" sentinel.
 */
#pragma once

#include <neograph/api.h>
#include <neograph/graph/cancel.h>

#include <asio/awaitable.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neograph {

/** Vector of independently reserved host resources. All quantities are non-negative units. */
struct NEOGRAPH_API HostResourceVector {
    std::uint64_t cpu_millis          = 0;
    std::uint64_t memory_bytes        = 0;
    std::uint64_t gpu_slots           = 0;
    std::uint64_t gpu_memory_bytes    = 0;
    std::uint64_t processes           = 0;
    std::uint64_t threads             = 0;
    std::uint64_t file_descriptors    = 0;
    std::uint64_t disk_bytes          = 0;
    std::uint64_t network_connections = 0;
    std::uint64_t tool_slots          = 0;
    std::uint64_t provider_requests   = 0;
    std::uint64_t model_tokens        = 0;
    std::uint64_t monetary_microunits = 0;
    std::uint64_t wall_time_ms        = 0;

    bool operator==(const HostResourceVector&) const = default;

    [[nodiscard]] bool empty() const noexcept;
    /** True only when every requested component is at most the corresponding limit. */
    [[nodiscard]] bool fits_within(const HostResourceVector& limit) const noexcept;
    [[nodiscard]] static HostResourceVector componentwise_min(const HostResourceVector& lhs,
                                                               const HostResourceVector& rhs) noexcept;
    /** Saturating addition prevents an impossible request from wrapping into admission. */
    [[nodiscard]] static HostResourceVector saturating_add(const HostResourceVector& lhs,
                                                            const HostResourceVector& rhs) noexcept;
    /** Clamped subtraction is used only to expose an available diagnostic snapshot. */
    [[nodiscard]] static HostResourceVector subtract_clamped(const HostResourceVector& lhs,
                                                              const HostResourceVector& rhs) noexcept;
};

enum class HostResourceConfidence : std::uint8_t {
    Measured,
    Estimated,
    ConservativeFallback,
};

NEOGRAPH_API std::string_view to_string(HostResourceConfidence confidence) noexcept;

/** Provenance pinned with a profile so later scheduling decisions are auditable. */
struct NEOGRAPH_API HostResourceEvidence {
    std::string                 source;
    HostResourceConfidence      confidence = HostResourceConfidence::ConservativeFallback;
    std::int64_t                observed_at_ms = 0;
    bool                        cgroup_limited = false;

    bool operator==(const HostResourceEvidence&) const = default;
};

/** Construction input for an immutable local host resource profile. */
struct NEOGRAPH_API HostResourceProfileData {
    /// Host-controlled generation token; it changes whenever configured limits change.
    std::string                 profile_id;
    HostResourceVector          capacity;
    /// Never admitted. It reserves host headroom for the operating system and recovery.
    HostResourceVector          safety_reserve;
    HostResourceEvidence        evidence;
};

/**
 * Immutable capacity snapshot.  Capacity after safety reserve is the only
 * quantity visible to the admission scheduler.
 */
class NEOGRAPH_API HostResourceProfile {
public:
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    static HostResourceProfile create(HostResourceProfileData data);
    /**
     * Detect cgroup-v2 / process limits on Linux and return conservative,
     * bounded fallbacks elsewhere.  It never invents GPU or network capacity.
     */
    static HostResourceProfile detect_current();
    /** Intersect independently enforced sources; the tightest limit wins per dimension. */
    static HostResourceProfile intersect(const std::vector<HostResourceProfile>& profiles,
                                         std::string profile_id);

    const std::string&          profile_id() const noexcept;
    const HostResourceVector&   capacity() const noexcept;
    const HostResourceVector&   safety_reserve() const noexcept;
    HostResourceVector          available_capacity() const noexcept;
    const HostResourceEvidence& evidence() const noexcept;
    std::string                 serialize_canonical() const;

private:
    explicit HostResourceProfile(HostResourceProfileData data);
    HostResourceProfileData data_;
};

enum class HostAdmissionFailure : std::uint8_t {
    InvalidRequest,
    CapacityExceeded,
    QueueFull,
    QueueTimeout,
    Shutdown,
};

NEOGRAPH_API std::string_view to_string(HostAdmissionFailure failure) noexcept;

class NEOGRAPH_API HostAdmissionError final : public std::runtime_error {
public:
    HostAdmissionError(HostAdmissionFailure failure, std::string detail);
    [[nodiscard]] HostAdmissionFailure failure() const noexcept;

private:
    HostAdmissionFailure failure_;
};

/** One atomic request to hold a vector of local host resources. */
struct NEOGRAPH_API HostAdmissionRequest {
    std::string                 owner_scope;
    std::string                 operation_id;
    HostResourceVector          resources;
    /// Higher values win once aging is included.  255 is the strongest static priority.
    std::uint8_t                priority = 0;
    /// Zero inherits HostAdmissionControllerConfig::max_pending.
    std::uint32_t               max_pending = 0;
    std::chrono::milliseconds   queue_timeout{0};
};

struct NEOGRAPH_API HostAdmissionSnapshot {
    HostResourceProfile profile = HostResourceProfile::detect_current();
    HostResourceVector  reserved;
    HostResourceVector  available;
    std::uint32_t       queued = 0;
    bool                overcommitted = false;
};

struct NEOGRAPH_API HostAdmissionControllerConfig {
    std::optional<HostResourceProfile> profile;
    std::uint32_t                      max_pending = 1024;
    /** A queued request gains one priority point per quantum, capped at 255. */
    std::chrono::milliseconds          aging_quantum{100};
};

namespace detail {
class HostAdmissionControllerImpl;
}

/** RAII lease. Destruction returns every reserved component to the scheduler. */
class NEOGRAPH_API HostResourceLease {
public:
    HostResourceLease() = default;
    HostResourceLease(const HostResourceLease&) = delete;
    HostResourceLease& operator=(const HostResourceLease&) = delete;
    HostResourceLease(HostResourceLease&& other) noexcept;
    HostResourceLease& operator=(HostResourceLease&& other) noexcept;
    ~HostResourceLease();

    [[nodiscard]] bool held() const noexcept;
    [[nodiscard]] const HostResourceVector& resources() const noexcept;
    /** Dynamic priority including a waiter that is blocked by this lease. */
    [[nodiscard]] std::uint8_t priority_hint() const noexcept;
    void release() noexcept;

private:
    friend class HostAdmissionController;
    HostResourceLease(std::shared_ptr<detail::HostAdmissionControllerImpl> impl,
                      std::uint64_t reservation_id,
                      HostResourceVector resources) noexcept;

    std::shared_ptr<detail::HostAdmissionControllerImpl> impl_;
    std::uint64_t                                        reservation_id_ = 0;
    HostResourceVector                                   resources_;
};

/**
 * Shared asynchronous capacity scheduler.
 *
 * Queued requests are selected by aging priority then FIFO sequence.  A held
 * lease receives the strongest priority of feasible work it blocks, allowing a
 * cooperative executor to prioritize the actual resource owner and avoid
 * priority inversion.  Capacity changes affect only new reservations; held
 * leases remain valid and an overcommitted snapshot blocks further grants.
 */
class NEOGRAPH_API HostAdmissionController {
public:
    explicit HostAdmissionController(HostAdmissionControllerConfig config = {});
    ~HostAdmissionController();

    HostAdmissionController(const HostAdmissionController&) = delete;
    HostAdmissionController& operator=(const HostAdmissionController&) = delete;

    asio::awaitable<HostResourceLease> reserve_async(
        HostAdmissionRequest request,
        std::shared_ptr<graph::CancelToken> cancel_token = {});

    /** Non-blocking admission; never bypasses an existing waiter. */
    std::optional<HostResourceLease> try_reserve(HostAdmissionRequest request);

    void update_profile(HostResourceProfile profile);
    [[nodiscard]] HostAdmissionSnapshot snapshot() const;
    void shutdown() noexcept;

private:
    std::shared_ptr<detail::HostAdmissionControllerImpl> impl_;
};

}  // namespace neograph
