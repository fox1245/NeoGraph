// The clock of the Program and host-loop runtime (#671). Reads std::chrono::steady_clock and
// std::chrono::system_clock unless a test has installed a SimulatedClock, whose steady and system
// times advance only when the test calls advance(). A HostLoop under a simulated clock needs a host
// executor whose now() follows that clock, as the tests' manual executor does: the loop schedules its
// host timer in the host's time.
#pragma once

#include <neograph/api.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <ratio>
#include <type_traits>

namespace neograph::async {
namespace detail {

class SimulatedClock final {
public:
    using steady_time_point = std::chrono::steady_clock::time_point;
    using steady_duration   = std::chrono::steady_clock::duration;
    using system_time_point = std::chrono::system_clock::time_point;
    using system_duration   = std::chrono::system_clock::duration;

    // The test chooses both start times; they move together from there.
    SimulatedClock(steady_time_point initial_steady, system_time_point initial_system) noexcept
        : steady_ticks_(initial_steady.time_since_epoch().count()),
          system_ticks_(initial_system.time_since_epoch().count()) {}

    SimulatedClock(const SimulatedClock&)            = delete;
    SimulatedClock& operator=(const SimulatedClock&) = delete;

    steady_time_point steady_now() const noexcept {
        return steady_time_point(steady_duration(steady_ticks_.load(std::memory_order_acquire)));
    }

    system_time_point system_now() const noexcept {
        return system_time_point(system_duration(system_ticks_.load(std::memory_order_acquire)));
    }

    // Advances both clocks by `delta`, saturating each tick counter at its numeric limits so that signed tick
    // arithmetic never overflows. The steady and system times move in two separate atomic steps, so a reader on
    // another thread can see steady_now() advanced before system_now(); tests advance the clock from the thread
    // that pumps the loop.
    template <typename Rep, typename Period>
    void advance(std::chrono::duration<Rep, Period> delta) noexcept {
        saturating_add(steady_ticks_, saturating_ticks<steady_duration>(delta));
        saturating_add(system_ticks_, saturating_ticks<system_duration>(delta));
    }

private:
    template <typename TargetDuration, typename Rep, typename Period>
    static typename TargetDuration::rep saturating_ticks(std::chrono::duration<Rep, Period> delta) noexcept {
        using target_rep       = typename TargetDuration::rep;
        using ratio            = std::ratio_divide<Period, typename TargetDuration::period>;
        constexpr auto max_rep = (std::numeric_limits<target_rep>::max)();
        constexpr auto min_rep = (std::numeric_limits<target_rep>::min)();
        if constexpr (std::is_floating_point_v<Rep>) {
            const auto raw = delta.count();
            if (!(raw == raw) || raw == static_cast<Rep>(0)) return 0;
            const long double scaled =
                static_cast<long double>(raw) * static_cast<long double>(ratio::num) /
                static_cast<long double>(ratio::den);
            if (scaled >= static_cast<long double>(max_rep)) return max_rep;
            if (scaled <= static_cast<long double>(min_rep)) return min_rep;
            return static_cast<target_rep>(scaled);
        } else {
            const auto raw = delta.count();
            if (raw == 0) return 0;
            constexpr auto num = static_cast<std::uintmax_t>(ratio::num);
            constexpr auto den = static_cast<std::uintmax_t>(ratio::den);
            if (raw > 0) {
                const auto     mag       = static_cast<std::uintmax_t>(raw);
                constexpr auto max_ticks = static_cast<std::uintmax_t>(max_rep);
                const auto     q         = mag / den;
                const auto     r         = mag % den;
                if (q > max_ticks / num) return max_rep;
                const auto     base  = q * num;
                std::uintmax_t extra = 0;
                if (r != 0) {
                    if (r > (std::numeric_limits<std::uintmax_t>::max)() / num) return max_rep;
                    extra = (r * num) / den;
                }
                if (extra > max_ticks - base) return max_rep;
                return static_cast<target_rep>(base + extra);
            }
            if constexpr (std::is_signed_v<Rep>) {
                const std::uintmax_t mag =
                    static_cast<std::uintmax_t>(-static_cast<std::intmax_t>(raw + 1)) + 1U;
                constexpr auto max_mag =
                    static_cast<std::uintmax_t>(-static_cast<std::intmax_t>(min_rep + 1)) + 1U;
                const auto q = mag / den;
                const auto r = mag % den;
                if (q > max_mag / num) return min_rep;
                const auto     base  = q * num;
                std::uintmax_t extra = 0;
                if (r != 0) {
                    if (r > (std::numeric_limits<std::uintmax_t>::max)() / num) return min_rep;
                    extra = (r * num) / den;
                }
                if (extra > max_mag - base) return min_rep;
                const std::uintmax_t total = base + extra;
                if (total == 0) return 0;
                if (total == max_mag) return min_rep;
                return -static_cast<target_rep>(total);
            } else {
                return 0;
            }
        }
    }

    template <typename Rep>
    static void saturating_add(std::atomic<Rep>& ticks, Rep delta) noexcept {
        constexpr auto max_rep = (std::numeric_limits<Rep>::max)();
        constexpr auto min_rep = (std::numeric_limits<Rep>::min)();
        auto           current = ticks.load(std::memory_order_relaxed);
        for (;;) {
            Rep next = current;
            if (delta > 0 && current > max_rep - delta) {
                next = max_rep;
            } else if (delta < 0 && current < min_rep - delta) {
                next = min_rep;
            } else {
                next = current + delta;
            }
            if (ticks.compare_exchange_weak(
                    current, next, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
        }
    }

    std::atomic<steady_duration::rep> steady_ticks_;
    std::atomic<system_duration::rep> system_ticks_;
};

extern NEOGRAPH_API std::atomic<SimulatedClock*> simulated_clock;

inline SimulatedClock* simulated_clock_for_testing() noexcept {
    return simulated_clock.load(std::memory_order_acquire);
}

NEOGRAPH_API void set_simulated_clock_for_testing(SimulatedClock* clock) noexcept;

}  // namespace detail

struct RuntimeClock {
    using duration   = std::chrono::steady_clock::duration;
    using rep        = duration::rep;
    using period     = duration::period;
    using time_point = std::chrono::steady_clock::time_point;

    static constexpr bool is_steady = std::chrono::steady_clock::is_steady;

    static time_point now() noexcept {
        if (const auto* clock = detail::simulated_clock.load(std::memory_order_acquire)) {
            return clock->steady_now();
        }
        return std::chrono::steady_clock::now();
    }
};

inline std::chrono::system_clock::time_point system_now() noexcept {
    if (const auto* clock = detail::simulated_clock.load(std::memory_order_acquire)) {
        return clock->system_now();
    }
    return std::chrono::system_clock::now();
}

}  // namespace neograph::async

namespace neograph::program::detail {
using async::RuntimeClock;
using async::system_now;
using async::detail::SimulatedClock;
using async::detail::set_simulated_clock_for_testing;
using async::detail::simulated_clock_for_testing;
}  // namespace neograph::program::detail
