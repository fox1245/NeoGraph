#include <neograph/host_admission.h>

#include <asio/error.hpp>
#include <asio/redirect_error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace neograph {
namespace {

constexpr std::uint32_t kMaximumPendingRequests = 65536;
constexpr auto kMaximumQueueTimeout = std::chrono::hours{24};
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

bool valid_token(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-' || character == '_' || character == '.' || character == ':';
    });
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) return std::nullopt;
    std::ostringstream value;
    value << stream.rdbuf();
    return value.str();
}

std::optional<std::uint64_t> parse_positive_limit(std::string value) {
    value = trim(std::move(value));
    if (value == "max") return std::nullopt;
    std::uint64_t parsed = 0;
    const auto [where, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || where != value.data() + value.size() || parsed == 0) {
        return std::nullopt;
    }
    return parsed;
}

std::uint64_t saturating_component_add(const std::uint64_t lhs, const std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
}

std::uint64_t clamped_component_subtract(const std::uint64_t lhs,
                                         const std::uint64_t rhs) noexcept {
    return lhs > rhs ? lhs - rhs : 0;
}

std::array<std::uint64_t, 14> components(const HostResourceVector& value) noexcept {
    return {value.cpu_millis, value.memory_bytes, value.gpu_slots, value.gpu_memory_bytes,
            value.processes, value.threads, value.file_descriptors, value.disk_bytes,
            value.network_connections, value.tool_slots, value.provider_requests,
            value.model_tokens, value.monetary_microunits, value.wall_time_ms};
}

HostResourceVector from_components(const std::array<std::uint64_t, 14>& values) noexcept {
    return {values[0], values[1], values[2], values[3], values[4], values[5], values[6],
            values[7], values[8], values[9], values[10], values[11], values[12], values[13]};
}

void append_vector(std::ostringstream& stream, const HostResourceVector& value) {
    stream << "\"cpu_millis\":" << value.cpu_millis
           << ",\"memory_bytes\":" << value.memory_bytes
           << ",\"gpu_slots\":" << value.gpu_slots
           << ",\"gpu_memory_bytes\":" << value.gpu_memory_bytes
           << ",\"processes\":" << value.processes
           << ",\"threads\":" << value.threads
           << ",\"file_descriptors\":" << value.file_descriptors
           << ",\"disk_bytes\":" << value.disk_bytes
           << ",\"network_connections\":" << value.network_connections
           << ",\"tool_slots\":" << value.tool_slots
           << ",\"provider_requests\":" << value.provider_requests
           << ",\"model_tokens\":" << value.model_tokens
           << ",\"monetary_microunits\":" << value.monetary_microunits
           << ",\"wall_time_ms\":" << value.wall_time_ms;
}

std::int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::uint64_t host_memory_bytes() {
#if defined(__linux__)
    if (const auto contents = read_file("/proc/meminfo")) {
        std::istringstream lines(*contents);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.starts_with("MemTotal:")) continue;
            std::istringstream parser(line.substr(std::string_view("MemTotal:").size()));
            std::uint64_t kibibytes = 0;
            if (parser >> kibibytes && kibibytes <= std::numeric_limits<std::uint64_t>::max() / 1024) {
                return kibibytes * 1024;
            }
        }
    }
#endif
    return 512 * kMiB;
}

std::uint64_t fallback_file_descriptors() {
#if defined(__linux__)
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY) {
        return static_cast<std::uint64_t>(limit.rlim_cur);
    }
#endif
    return 256;
}

std::filesystem::path cgroup_v2_path() {
#if defined(__linux__)
    const std::filesystem::path mount{"/sys/fs/cgroup"};
    std::error_code error;
    if (!std::filesystem::exists(mount / "cgroup.controllers", error) || error) return {};
    const auto membership = read_file("/proc/self/cgroup");
    if (!membership) return mount;
    std::istringstream lines(*membership);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.starts_with("0::")) continue;
        const auto relative = trim(line.substr(3));
        if (relative.empty() || relative == "/") return mount;
        const auto path = mount / relative.substr(relative.front() == '/' ? 1 : 0);
        if (std::filesystem::exists(path, error) && !error) return path;
    }
    return mount;
#else
    return {};
#endif
}

std::uint64_t cpu_fallback_millis() {
    const auto hardware = std::max(1U, std::thread::hardware_concurrency());
    return static_cast<std::uint64_t>(hardware) * 1000;
}

std::uint64_t cgroup_cpu_millis(const std::filesystem::path& path,
                                const std::uint64_t fallback) {
    const auto contents = read_file(path / "cpu.max");
    if (!contents) return fallback;
    std::istringstream parser(*contents);
    std::string quota;
    std::uint64_t period = 0;
    if (!(parser >> quota >> period) || quota == "max" || period == 0) return fallback;
    const auto parsed = parse_positive_limit(quota);
    if (!parsed || *parsed > std::numeric_limits<std::uint64_t>::max() / 1000) return fallback;
    return std::max<std::uint64_t>(1, *parsed * 1000 / period);
}

} // namespace

bool HostResourceVector::empty() const noexcept {
    const auto values = components(*this);
    return std::all_of(values.begin(), values.end(), [](const auto value) { return value == 0; });
}

bool HostResourceVector::fits_within(const HostResourceVector& limit) const noexcept {
    const auto requested = components(*this);
    const auto available = components(limit);
    for (std::size_t index = 0; index < requested.size(); ++index) {
        if (requested[index] > available[index]) return false;
    }
    return true;
}

HostResourceVector HostResourceVector::componentwise_min(const HostResourceVector& lhs,
                                                          const HostResourceVector& rhs) noexcept {
    auto left = components(lhs);
    const auto right = components(rhs);
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = std::min(left[index], right[index]);
    }
    return from_components(left);
}

HostResourceVector HostResourceVector::saturating_add(const HostResourceVector& lhs,
                                                       const HostResourceVector& rhs) noexcept {
    auto left = components(lhs);
    const auto right = components(rhs);
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = saturating_component_add(left[index], right[index]);
    }
    return from_components(left);
}

HostResourceVector HostResourceVector::subtract_clamped(const HostResourceVector& lhs,
                                                         const HostResourceVector& rhs) noexcept {
    auto left = components(lhs);
    const auto right = components(rhs);
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = clamped_component_subtract(left[index], right[index]);
    }
    return from_components(left);
}

std::string_view to_string(const HostResourceConfidence confidence) noexcept {
    switch (confidence) {
        case HostResourceConfidence::Measured: return "measured";
        case HostResourceConfidence::Estimated: return "estimated";
        case HostResourceConfidence::ConservativeFallback: return "conservative_fallback";
    }
    return "unknown";
}

HostResourceProfile::HostResourceProfile(HostResourceProfileData data)
    : data_(std::move(data)) {}

HostResourceProfile HostResourceProfile::create(HostResourceProfileData data) {
    if (!valid_token(data.profile_id)) {
        throw std::invalid_argument("host resource profile_id must be a non-empty safe token");
    }
    if (!valid_token(data.evidence.source)) {
        throw std::invalid_argument("host resource evidence source must be a non-empty safe token");
    }
    if (data.evidence.observed_at_ms < 0) {
        throw std::invalid_argument("host resource evidence observed_at_ms must not be negative");
    }
    if (!data.safety_reserve.fits_within(data.capacity)) {
        throw std::invalid_argument("host resource safety reserve exceeds configured capacity");
    }
    return HostResourceProfile(std::move(data));
}

HostResourceProfile HostResourceProfile::detect_current() {
    HostResourceProfileData data;
    data.evidence.observed_at_ms = now_ms();
    data.capacity.cpu_millis = cpu_fallback_millis();
    data.capacity.memory_bytes = host_memory_bytes();
    data.capacity.processes = 64;
    data.capacity.threads = 64;
    data.capacity.file_descriptors = fallback_file_descriptors();

    std::error_code disk_error;
    const auto temporary_path = std::filesystem::temp_directory_path(disk_error);
    if (!disk_error) {
        const auto disk = std::filesystem::space(temporary_path, disk_error);
        if (!disk_error) data.capacity.disk_bytes = disk.available;
    }

    const auto cgroup = cgroup_v2_path();
    if (!cgroup.empty()) {
        data.evidence.source = "linux-cgroup-v2";
        data.evidence.confidence = HostResourceConfidence::Measured;
        data.evidence.cgroup_limited = true;
        data.capacity.cpu_millis = cgroup_cpu_millis(cgroup, data.capacity.cpu_millis);
        if (const auto contents = read_file(cgroup / "memory.max")) {
            if (const auto memory = parse_positive_limit(*contents)) {
                data.capacity.memory_bytes = *memory;
            }
        }
        if (const auto contents = read_file(cgroup / "pids.max")) {
            if (const auto processes = parse_positive_limit(*contents)) {
                data.capacity.processes = *processes;
                data.capacity.threads = *processes;
            }
        }
    } else {
        data.evidence.source = "process-limits";
        data.evidence.confidence = HostResourceConfidence::ConservativeFallback;
    }

    // GPU and external-network capacity are deliberately zero until a host
    // profile declares them.  Unknown is not treated as unlimited permission.
    data.capacity.tool_slots = std::max<std::uint64_t>(1, data.capacity.cpu_millis / 1000);
    data.capacity.provider_requests = data.capacity.tool_slots;
    data.safety_reserve.memory_bytes = data.capacity.memory_bytes / 8;
    data.safety_reserve.processes = data.capacity.processes / 16;
    data.safety_reserve.threads = data.capacity.threads / 16;
    data.safety_reserve.file_descriptors = std::min<std::uint64_t>(64, data.capacity.file_descriptors / 8);
    data.safety_reserve.disk_bytes = data.capacity.disk_bytes / 20;
    data.profile_id = "detected-" + std::to_string(data.evidence.observed_at_ms);
    return create(std::move(data));
}

HostResourceProfile HostResourceProfile::intersect(const std::vector<HostResourceProfile>& profiles,
                                                    std::string profile_id) {
    if (profiles.empty()) throw std::invalid_argument("cannot intersect an empty host profile set");
    if (!valid_token(profile_id)) throw std::invalid_argument("host intersection profile_id is invalid");

    auto capacity = profiles.front().available_capacity();
    auto confidence = profiles.front().evidence().confidence;
    auto observed_at_ms = profiles.front().evidence().observed_at_ms;
    auto cgroup_limited = profiles.front().evidence().cgroup_limited;
    for (std::size_t index = 1; index < profiles.size(); ++index) {
        capacity = HostResourceVector::componentwise_min(capacity, profiles[index].available_capacity());
        confidence = std::max(confidence, profiles[index].evidence().confidence);
        observed_at_ms = std::min(observed_at_ms, profiles[index].evidence().observed_at_ms);
        cgroup_limited = cgroup_limited || profiles[index].evidence().cgroup_limited;
    }

    HostResourceProfileData data;
    data.profile_id = std::move(profile_id);
    // Capacity is already post-reserve. Reapplying source reserves would weaken
    // the tightest source twice and make the intersection non-monotonic.
    data.capacity = capacity;
    data.evidence = {"profile-intersection", confidence, observed_at_ms, cgroup_limited};
    return create(std::move(data));
}

const std::string& HostResourceProfile::profile_id() const noexcept { return data_.profile_id; }
const HostResourceVector& HostResourceProfile::capacity() const noexcept { return data_.capacity; }
const HostResourceVector& HostResourceProfile::safety_reserve() const noexcept { return data_.safety_reserve; }
HostResourceVector HostResourceProfile::available_capacity() const noexcept {
    return HostResourceVector::subtract_clamped(data_.capacity, data_.safety_reserve);
}
const HostResourceEvidence& HostResourceProfile::evidence() const noexcept { return data_.evidence; }

std::string HostResourceProfile::serialize_canonical() const {
    std::ostringstream stream;
    stream << "{\"format\":\"neograph-host-resource-profile\",\"schema_version\":" << SCHEMA_VERSION
           << ",\"profile_id\":\"" << data_.profile_id << "\",\"capacity\":{";
    append_vector(stream, data_.capacity);
    stream << "},\"safety_reserve\":{";
    append_vector(stream, data_.safety_reserve);
    stream << "},\"evidence\":{\"source\":\"" << data_.evidence.source
           << "\",\"confidence\":\"" << to_string(data_.evidence.confidence)
           << "\",\"observed_at_ms\":" << data_.evidence.observed_at_ms
           << ",\"cgroup_limited\":" << (data_.evidence.cgroup_limited ? "true" : "false") << "}}";
    return stream.str();
}

std::string_view to_string(const HostAdmissionFailure failure) noexcept {
    switch (failure) {
        case HostAdmissionFailure::InvalidRequest: return "invalid_request";
        case HostAdmissionFailure::CapacityExceeded: return "capacity_exceeded";
        case HostAdmissionFailure::QueueFull: return "queue_full";
        case HostAdmissionFailure::QueueTimeout: return "queue_timeout";
        case HostAdmissionFailure::Shutdown: return "shutdown";
    }
    return "unknown";
}

HostAdmissionError::HostAdmissionError(const HostAdmissionFailure failure, std::string detail)
    : std::runtime_error("neograph host admission " + std::string(to_string(failure)) + ": " + detail),
      failure_(failure) {}

HostAdmissionFailure HostAdmissionError::failure() const noexcept { return failure_; }

namespace detail {

class HostAdmissionControllerImpl final : public std::enable_shared_from_this<HostAdmissionControllerImpl> {
public:
    enum class WaiterState : std::uint8_t { Queued, Granted, Claimed, Cancelled };

    struct Reservation {
        HostResourceVector resources;
        std::uint8_t base_priority = 0;
        std::uint8_t inherited_priority = 0;
    };

    struct Waiter {
        HostAdmissionRequest request;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point submitted_at;
        std::uint64_t reservation_id = 0;
        WaiterState state = WaiterState::Queued;
    };

    struct Registration {
        std::uint64_t reservation_id = 0;
        std::shared_ptr<Waiter> waiter;
        HostResourceVector resources;
    };

    explicit HostAdmissionControllerImpl(HostAdmissionControllerConfig config)
        : profile_(config.profile.value_or(HostResourceProfile::detect_current())),
          max_pending_(config.max_pending),
          aging_quantum_(config.aging_quantum) {
        if (max_pending_ == 0 || max_pending_ > kMaximumPendingRequests) {
            throw std::invalid_argument("host admission max_pending must be in 1..65536");
        }
        if (aging_quantum_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("host admission aging_quantum must be positive");
        }
    }

    Registration enqueue(HostAdmissionRequest request) {
        normalize_request(request, true);
        std::lock_guard lock(mu_);
        ensure_running_locked();
        ensure_request_can_ever_fit_locked(request);

        const auto now = std::chrono::steady_clock::now();
        if (waiters_.empty() && fits_locked(request.resources)) {
            const auto resources = request.resources;
            return {reserve_locked(request), {}, resources};
        }
        if (waiters_.size() >= request.max_pending) {
            throw HostAdmissionError(HostAdmissionFailure::QueueFull, "pending queue is full");
        }
        auto waiter = std::make_shared<Waiter>();
        waiter->request = std::move(request);
        waiter->sequence = next_sequence_++;
        waiter->submitted_at = now;
        waiters_.push_back(waiter);
        schedule_locked(now);
        if (waiter->state == WaiterState::Granted) {
            return {waiter->reservation_id, std::move(waiter), {}};
        }
        return {0, std::move(waiter), {}};
    }

    std::optional<std::uint64_t> try_reserve(HostAdmissionRequest request) {
        normalize_request(request, false);
        std::lock_guard lock(mu_);
        ensure_running_locked();
        ensure_request_can_ever_fit_locked(request);
        if (!waiters_.empty() || !fits_locked(request.resources)) return std::nullopt;
        return reserve_locked(request);
    }

    [[nodiscard]] WaiterState state(const std::shared_ptr<Waiter>& waiter) const noexcept {
        std::lock_guard lock(mu_);
        return waiter->state;
    }

    std::uint64_t claim(const std::shared_ptr<Waiter>& waiter) {
        std::lock_guard lock(mu_);
        if (waiter->state != WaiterState::Granted) {
            throw HostAdmissionError(HostAdmissionFailure::Shutdown, "queued host reservation was revoked");
        }
        waiter->state = WaiterState::Claimed;
        return waiter->reservation_id;
    }

    void abandon(const std::shared_ptr<Waiter>& waiter) noexcept {
        std::lock_guard lock(mu_);
        if (waiter->state == WaiterState::Queued) {
            const auto found = std::find(waiters_.begin(), waiters_.end(), waiter);
            if (found != waiters_.end()) waiters_.erase(found);
            waiter->state = WaiterState::Cancelled;
            schedule_locked(std::chrono::steady_clock::now());
        } else if (waiter->state == WaiterState::Granted) {
            release_locked(waiter->reservation_id);
            waiter->state = WaiterState::Cancelled;
        }
    }

    void release(const std::uint64_t reservation_id) noexcept {
        std::lock_guard lock(mu_);
        release_locked(reservation_id);
    }

    [[nodiscard]] std::uint8_t priority_hint(const std::uint64_t reservation_id) const noexcept {
        std::lock_guard lock(mu_);
        const auto found = reservations_.find(reservation_id);
        if (found == reservations_.end()) return 0;

        auto priority = found->second.base_priority;
        const auto available =
            HostResourceVector::subtract_clamped(profile_.available_capacity(), reserved_);
        const auto available_components = components(available);
        const auto held_components = components(found->second.resources);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& waiter : waiters_) {
            if (waiter->state != WaiterState::Queued) continue;
            const auto requested = components(waiter->request.resources);
            for (std::size_t index = 0; index < requested.size(); ++index) {
                if (requested[index] > available_components[index] && held_components[index] > 0) {
                    priority = std::max(priority, effective_priority(*waiter, now));
                    break;
                }
            }
        }
        return priority;
    }

    void update_profile(HostResourceProfile profile) {
        std::lock_guard lock(mu_);
        profile_ = std::move(profile);
        if (!stopping_) schedule_locked(std::chrono::steady_clock::now());
    }

    [[nodiscard]] HostAdmissionSnapshot snapshot() const {
        std::lock_guard lock(mu_);
        const auto effective_capacity = profile_.available_capacity();
        return {profile_, reserved_, HostResourceVector::subtract_clamped(effective_capacity, reserved_),
                static_cast<std::uint32_t>(waiters_.size()), !reserved_.fits_within(effective_capacity)};
    }

    void shutdown() noexcept {
        std::lock_guard lock(mu_);
        if (stopping_) return;
        stopping_ = true;
        for (auto& waiter : waiters_) waiter->state = WaiterState::Cancelled;
        waiters_.clear();
        recompute_inheritance_locked(std::chrono::steady_clock::now());
    }

private:
    void normalize_request(HostAdmissionRequest& request, const bool blocking) const {
        if (request.resources.empty()) {
            throw HostAdmissionError(HostAdmissionFailure::InvalidRequest,
                                     "a host reservation must contain at least one resource");
        }
        if (request.owner_scope.empty()) request.owner_scope = "anonymous";
        if (request.operation_id.empty()) request.operation_id = "unspecified";
        if (request.owner_scope.size() > 512 || request.operation_id.size() > 512) {
            throw HostAdmissionError(HostAdmissionFailure::InvalidRequest,
                                     "owner_scope and operation_id must be at most 512 bytes");
        }
        if (request.max_pending == 0) request.max_pending = max_pending_;
        if (request.max_pending > max_pending_) {
            throw HostAdmissionError(HostAdmissionFailure::InvalidRequest,
                                     "request max_pending exceeds host controller limit");
        }
        if (blocking
            && (request.queue_timeout <= std::chrono::milliseconds::zero()
                || request.queue_timeout > kMaximumQueueTimeout)) {
            throw HostAdmissionError(HostAdmissionFailure::InvalidRequest,
                                     "queue_timeout must be positive and at most 24 hours");
        }
    }

    void ensure_running_locked() const {
        if (stopping_) throw HostAdmissionError(HostAdmissionFailure::Shutdown, "controller is shut down");
    }

    void ensure_request_can_ever_fit_locked(const HostAdmissionRequest& request) const {
        if (!request.resources.fits_within(profile_.available_capacity())) {
            throw HostAdmissionError(HostAdmissionFailure::CapacityExceeded,
                                     "requested resource vector exceeds host capacity");
        }
    }

    [[nodiscard]] bool fits_locked(const HostResourceVector& requested) const noexcept {
        return HostResourceVector::saturating_add(reserved_, requested)
            .fits_within(profile_.available_capacity());
    }

    std::uint64_t reserve_locked(const HostAdmissionRequest& request) {
        const auto reservation_id = next_reservation_id_++;
        reserved_ = HostResourceVector::saturating_add(reserved_, request.resources);
        reservations_.emplace(reservation_id,
                              Reservation{request.resources, request.priority, request.priority});
        recompute_inheritance_locked(std::chrono::steady_clock::now());
        return reservation_id;
    }

    [[nodiscard]] std::uint8_t effective_priority(
        const Waiter& waiter,
        const std::chrono::steady_clock::time_point now) const noexcept {
        const auto elapsed = now > waiter.submitted_at
            ? std::chrono::duration_cast<std::chrono::milliseconds>(now - waiter.submitted_at)
            : std::chrono::milliseconds::zero();
        const auto gains = static_cast<std::uint64_t>(elapsed.count())
            / static_cast<std::uint64_t>(aging_quantum_.count());
        return static_cast<std::uint8_t>(
            std::min<std::uint64_t>(255, static_cast<std::uint64_t>(waiter.request.priority) + gains));
    }

    void schedule_locked(const std::chrono::steady_clock::time_point now) {
        if (stopping_) return;
        for (;;) {
            auto selected = waiters_.end();
            std::uint8_t selected_priority = 0;
            for (auto current = waiters_.begin(); current != waiters_.end(); ++current) {
                const auto& waiter = *current;
                if (waiter->state != WaiterState::Queued || !fits_locked(waiter->request.resources)) continue;
                const auto priority = effective_priority(*waiter, now);
                if (selected == waiters_.end() || priority > selected_priority
                    || (priority == selected_priority && waiter->sequence < (*selected)->sequence)) {
                    selected = current;
                    selected_priority = priority;
                }
            }
            if (selected == waiters_.end()) break;
            const auto waiter = *selected;
            waiters_.erase(selected);
            waiter->reservation_id = reserve_locked(waiter->request);
            waiter->state = WaiterState::Granted;
        }
        recompute_inheritance_locked(now);
    }

    void release_locked(const std::uint64_t reservation_id) noexcept {
        const auto found = reservations_.find(reservation_id);
        if (found == reservations_.end()) return;
        const auto resources = found->second.resources;
        reservations_.erase(found);
        reserved_ = HostResourceVector::subtract_clamped(reserved_, resources);
        if (!stopping_) schedule_locked(std::chrono::steady_clock::now());
        else recompute_inheritance_locked(std::chrono::steady_clock::now());
    }

    void recompute_inheritance_locked(const std::chrono::steady_clock::time_point now) noexcept {
        for (auto& [_, reservation] : reservations_) {
            reservation.inherited_priority = reservation.base_priority;
        }
        const auto available = HostResourceVector::subtract_clamped(profile_.available_capacity(), reserved_);
        const auto available_components = components(available);
        for (const auto& waiter : waiters_) {
            if (waiter->state != WaiterState::Queued) continue;
            const auto requested = components(waiter->request.resources);
            const auto priority = effective_priority(*waiter, now);
            for (auto& [_, reservation] : reservations_) {
                const auto held = components(reservation.resources);
                bool contributes_to_shortage = false;
                for (std::size_t index = 0; index < requested.size(); ++index) {
                    if (requested[index] > available_components[index] && held[index] > 0) {
                        contributes_to_shortage = true;
                        break;
                    }
                }
                if (contributes_to_shortage) {
                    reservation.inherited_priority = std::max(reservation.inherited_priority, priority);
                }
            }
        }
    }

    mutable std::mutex mu_;
    HostResourceProfile profile_;
    HostResourceVector reserved_;
    std::unordered_map<std::uint64_t, Reservation> reservations_;
    std::deque<std::shared_ptr<Waiter>> waiters_;
    std::uint64_t next_reservation_id_ = 1;
    std::uint64_t next_sequence_ = 1;
    std::uint32_t max_pending_;
    std::chrono::milliseconds aging_quantum_;
    bool stopping_ = false;
};

} // namespace detail

HostResourceLease::HostResourceLease(std::shared_ptr<detail::HostAdmissionControllerImpl> impl,
                                     const std::uint64_t reservation_id,
                                     HostResourceVector resources) noexcept
    : impl_(std::move(impl)), reservation_id_(reservation_id), resources_(resources) {}

HostResourceLease::HostResourceLease(HostResourceLease&& other) noexcept
    : impl_(std::move(other.impl_)),
      reservation_id_(std::exchange(other.reservation_id_, 0)),
      resources_(std::exchange(other.resources_, HostResourceVector{})) {}

HostResourceLease& HostResourceLease::operator=(HostResourceLease&& other) noexcept {
    if (this != &other) {
        release();
        impl_ = std::move(other.impl_);
        reservation_id_ = std::exchange(other.reservation_id_, 0);
        resources_ = std::exchange(other.resources_, HostResourceVector{});
    }
    return *this;
}

HostResourceLease::~HostResourceLease() noexcept { release(); }

bool HostResourceLease::held() const noexcept { return static_cast<bool>(impl_); }
const HostResourceVector& HostResourceLease::resources() const noexcept { return resources_; }
std::uint8_t HostResourceLease::priority_hint() const noexcept {
    return impl_ ? impl_->priority_hint(reservation_id_) : 0;
}

void HostResourceLease::release() noexcept {
    if (!impl_) return;
    impl_->release(reservation_id_);
    impl_.reset();
    reservation_id_ = 0;
    resources_ = {};
}

HostAdmissionController::HostAdmissionController(HostAdmissionControllerConfig config)
    : impl_(std::make_shared<detail::HostAdmissionControllerImpl>(std::move(config))) {}

HostAdmissionController::~HostAdmissionController() { shutdown(); }

asio::awaitable<HostResourceLease> HostAdmissionController::reserve_async(
    HostAdmissionRequest request,
    std::shared_ptr<graph::CancelToken> cancel_token) {
    if (cancel_token) cancel_token->throw_if_cancelled("while waiting for host resources");

    struct PendingGuard {
        std::shared_ptr<detail::HostAdmissionControllerImpl> impl;
        std::shared_ptr<detail::HostAdmissionControllerImpl::Waiter> waiter;

        ~PendingGuard() {
            if (waiter) impl->abandon(waiter);
        }

        HostResourceLease claim() {
            const auto resources = waiter->request.resources;
            const auto reservation_id = impl->claim(waiter);
            waiter.reset();
            return HostResourceLease(impl, reservation_id, resources);
        }
    };

    auto registration = impl_->enqueue(std::move(request));
    if (!registration.waiter) {
        auto lease = HostResourceLease(impl_, registration.reservation_id,
                                       std::move(registration.resources));
        if (cancel_token) cancel_token->throw_if_cancelled("after host resource admission");
        co_return std::move(lease);
    }

    PendingGuard pending{impl_, std::move(registration.waiter)};
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer poll(executor);
    const auto deadline = std::chrono::steady_clock::now() + pending.waiter->request.queue_timeout;

    for (;;) {
        if (cancel_token && cancel_token->is_cancelled()) {
            throw graph::CancelledException("while waiting for host resources");
        }
        switch (impl_->state(pending.waiter)) {
            case detail::HostAdmissionControllerImpl::WaiterState::Granted:
                if (cancel_token) cancel_token->throw_if_cancelled("after host resource admission");
                co_return pending.claim();
            case detail::HostAdmissionControllerImpl::WaiterState::Cancelled:
                if (cancel_token && cancel_token->is_cancelled()) {
                    throw graph::CancelledException("while waiting for host resources");
                }
                throw HostAdmissionError(HostAdmissionFailure::Shutdown,
                                         "queued host reservation was cancelled");
            case detail::HostAdmissionControllerImpl::WaiterState::Claimed:
                throw std::logic_error("host reservation waiter was claimed twice");
            case detail::HostAdmissionControllerImpl::WaiterState::Queued:
                break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw HostAdmissionError(HostAdmissionFailure::QueueTimeout,
                                     "queued host reservation reached its deadline");
        }
        poll.expires_after(std::min(std::chrono::milliseconds{2},
                                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        asio::error_code error;
        co_await poll.async_wait(asio::redirect_error(asio::use_awaitable, error));
        if (error && error != asio::error::operation_aborted) throw asio::system_error(error);
        if (error == asio::error::operation_aborted) throw asio::system_error(error);
    }
}

std::optional<HostResourceLease> HostAdmissionController::try_reserve(HostAdmissionRequest request) {
    const auto resources = request.resources;
    const auto reservation_id = impl_->try_reserve(std::move(request));
    if (!reservation_id) return std::nullopt;
    return HostResourceLease(impl_, *reservation_id, resources);
}

void HostAdmissionController::update_profile(HostResourceProfile profile) {
    impl_->update_profile(std::move(profile));
}

HostAdmissionSnapshot HostAdmissionController::snapshot() const { return impl_->snapshot(); }
void HostAdmissionController::shutdown() noexcept { impl_->shutdown(); }

} // namespace neograph
