// Conservative, host-testable connectivity freshness tracking.
#pragma once

#include <cstdint>
#include <limits>

namespace nxpctl::connectivity {

inline constexpr std::uint64_t OfflineGraceSeconds = 120;

enum class State : std::uint32_t {
    Unknown = 0, // No authenticated server response since boot.
    Online = 1,
    Offline = 2,
};

[[nodiscard]] inline State GetState(
    bool has_successful_contact,
    std::uint64_t last_success_monotonic,
    std::uint64_t monotonic_now) noexcept {
    if (!has_successful_contact || last_success_monotonic == 0 ||
        monotonic_now < last_success_monotonic) {
        return State::Unknown;
    }
    return monotonic_now - last_success_monotonic <= OfflineGraceSeconds
        ? State::Online
        : State::Offline;
}

[[nodiscard]] inline bool IsOffline(
    bool has_successful_contact,
    std::uint64_t last_success_monotonic,
    std::uint64_t monotonic_now) noexcept {
    return GetState(has_successful_contact, last_success_monotonic, monotonic_now)
        != State::Online;
}

[[nodiscard]] inline std::uint64_t GetSuccessAge(
    bool has_successful_contact,
    std::uint64_t last_success_monotonic,
    std::uint64_t monotonic_now) noexcept {
    if (!has_successful_contact || last_success_monotonic == 0 ||
        monotonic_now < last_success_monotonic) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return monotonic_now - last_success_monotonic;
}

} // namespace nxpctl::connectivity
