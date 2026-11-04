// Derive local calendar-day keys from an authenticated server-time anchor.
#pragma once

#include <cstdint>
#include <limits>

namespace nxpctl::trusted_clock {

struct Anchor {
    std::int64_t server_time = 0;
    std::int64_t not_after = 0;
    std::uint64_t monotonic_seconds = 0;
    std::int16_t timezone_offset_minutes = 0;
    bool valid = false;
};

// Wall-clock time is never consulted. The anchor's authenticated server time
// advances only by elapsed monotonic seconds and expires with its policy.
[[nodiscard]] inline bool GetLocalDayKey(
    const Anchor& anchor,
    std::uint64_t monotonic_now,
    std::int64_t* out_day_key) noexcept {
    if (out_day_key == nullptr || !anchor.valid ||
        anchor.server_time <= 0 || anchor.not_after <= anchor.server_time ||
        anchor.timezone_offset_minutes < -14 * 60 ||
        anchor.timezone_offset_minutes > 14 * 60 ||
        monotonic_now < anchor.monotonic_seconds) {
        return false;
    }

    const std::uint64_t elapsed = monotonic_now - anchor.monotonic_seconds;
    const std::uint64_t lifetime =
        static_cast<std::uint64_t>(anchor.not_after - anchor.server_time);
    if (elapsed > lifetime ||
        elapsed > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max() - anchor.server_time)) {
        return false;
    }

    const std::int64_t utc_now =
        anchor.server_time + static_cast<std::int64_t>(elapsed);
    const std::int64_t offset_seconds =
        static_cast<std::int64_t>(anchor.timezone_offset_minutes) * 60;
    if ((offset_seconds > 0 &&
         utc_now > std::numeric_limits<std::int64_t>::max() - offset_seconds) ||
        (offset_seconds < 0 &&
         utc_now < std::numeric_limits<std::int64_t>::min() - offset_seconds)) {
        return false;
    }

    const std::int64_t local_now = utc_now + offset_seconds;
    if (local_now < 0) return false;

    *out_day_key = local_now / 86400;
    return true;
}

} // namespace nxpctl::trusted_clock
