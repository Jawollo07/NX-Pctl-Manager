// Monotonic, Horizon-independent global daily playtime accounting.
#pragma once

#include <cstdint>
#include <limits>

namespace nxpctl::usage {

inline constexpr std::uint64_t MaxObservationGapSeconds = 2;
inline constexpr std::uint16_t MaxDailyUsageMinutes = 24 * 60;

enum class ObserveResult : std::uint8_t {
    BaselineEstablished = 0,
    Accumulated,
    DayRolledOver,
    ClockRegressed,
    ObservationGapTooLarge,
};

class DailyUsageTracker {
public:
    // Counts total console application-active time, not time per title.
    // application_active is true while any game/application is running.
    // local_day_key is a monotonically increasing local calendar-day number
    // after applying the policy timezone offset. monotonic_seconds must come
    // from a monotonic clock, never wall time.
    [[nodiscard]] ObserveResult Observe(
        bool application_active,
        std::uint64_t monotonic_seconds,
        std::int64_t local_day_key) noexcept {
        if (!initialized_) {
            initialized_ = true;
            previous_application_was_active_ = application_active;
            last_monotonic_seconds_ = monotonic_seconds;
            local_day_key_ = local_day_key;
            return ObserveResult::BaselineEstablished;
        }

        if (local_day_key > local_day_key_) {
            // Do not charge an interval spanning midnight to either day.
            active_seconds_ = 0;
            local_day_key_ = local_day_key;
            previous_application_was_active_ = application_active;
            last_monotonic_seconds_ = monotonic_seconds;
            return ObserveResult::DayRolledOver;
        }

        if (local_day_key < local_day_key_ ||
            monotonic_seconds < last_monotonic_seconds_) {
            // A wall-clock correction must not roll the usage counter back.
            previous_application_was_active_ = application_active;
            last_monotonic_seconds_ = monotonic_seconds;
            return ObserveResult::ClockRegressed;
        }

        const std::uint64_t elapsed = monotonic_seconds - last_monotonic_seconds_;
        last_monotonic_seconds_ = monotonic_seconds;

        if (elapsed > MaxObservationGapSeconds) {
            // A stalled worker, suspend, or missed observation is not
            // assumed to be continuous playtime.
            previous_application_was_active_ = application_active;
            return ObserveResult::ObservationGapTooLarge;
        }

        if (previous_application_was_active_) {
            if (std::numeric_limits<std::uint64_t>::max() - active_seconds_ < elapsed) {
                active_seconds_ = std::numeric_limits<std::uint64_t>::max();
            } else {
                active_seconds_ += elapsed;
            }
        }

        previous_application_was_active_ = application_active;
        return ObserveResult::Accumulated;
    }

    void Reset() noexcept {
        initialized_ = false;
        previous_application_was_active_ = false;
        last_monotonic_seconds_ = 0;
        local_day_key_ = 0;
        active_seconds_ = 0;
    }

    [[nodiscard]] std::uint64_t GetActiveSeconds() const noexcept {
        return active_seconds_;
    }

    // Round up: any started minute consumes one policy minute.
    [[nodiscard]] std::uint16_t GetDailyUsageMinutes() const noexcept {
        if (active_seconds_ == 0) return 0;
        const std::uint64_t rounded = 1 + ((active_seconds_ - 1) / 60);
        return static_cast<std::uint16_t>(
            rounded > MaxDailyUsageMinutes ? MaxDailyUsageMinutes : rounded);
    }

    [[nodiscard]] std::int64_t GetLocalDayKey() const noexcept {
        return local_day_key_;
    }

    [[nodiscard]] bool HasBaseline() const noexcept {
        return initialized_;
    }

private:
    bool initialized_ = false;
    bool previous_application_was_active_ = false;
    std::uint64_t last_monotonic_seconds_ = 0;
    std::int64_t local_day_key_ = 0;
    std::uint64_t active_seconds_ = 0;
};

} // namespace nxpctl::usage
