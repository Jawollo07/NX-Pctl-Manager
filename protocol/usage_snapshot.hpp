// Validated, global daily-usage snapshot reconciliation shared with host tests.
#pragma once
#include <cstdint>
#include <limits>

namespace nxpctl::usage {

struct Snapshot {
    std::int64_t local_day_key = 0;
    std::uint64_t active_seconds = 0;
    std::uint64_t sequence = 0;
    bool valid = false;
};

inline constexpr std::uint64_t MaxPersistedActiveSeconds = 24 * 60 * 60;

[[nodiscard]] inline bool IsValidSnapshot(const Snapshot& value) noexcept {
    return value.valid && value.local_day_key >= 0 &&
           value.active_seconds <= MaxPersistedActiveSeconds &&
           value.sequence != 0;
}

[[nodiscard]] inline const Snapshot* SelectNewest(
    const Snapshot& first, const Snapshot& second) noexcept {
    const bool a = IsValidSnapshot(first);
    const bool b = IsValidSnapshot(second);
    if (!a && !b) return nullptr;
    if (!a) return &second;
    if (!b) return &first;
    return second.sequence > first.sequence ? &second : &first;
}

// Never roll persisted usage backwards if the trusted server clock regresses.
// A later trusted day starts a fresh bucket; the previous day is not carried.
[[nodiscard]] inline bool ReconcileSnapshot(
    const Snapshot& stored, std::int64_t trusted_day_key,
    std::int64_t* out_day_key, std::uint64_t* out_active_seconds) noexcept {
    if (out_day_key == nullptr || out_active_seconds == nullptr ||
        trusted_day_key < 0) return false;
    if (IsValidSnapshot(stored) && trusted_day_key < stored.local_day_key)
        return false;
    *out_day_key = trusted_day_key;
    *out_active_seconds =
        IsValidSnapshot(stored) && trusted_day_key == stored.local_day_key
            ? stored.active_seconds : 0;
    return true;
}

} // namespace nxpctl::usage
