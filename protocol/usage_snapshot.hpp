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
    if (first.sequence == second.sequence) {
        // Equal generations are safe only when they describe the same state.
        // Conflicting authenticated snapshots are ambiguous and must fail closed.
        if (first.local_day_key != second.local_day_key ||
            first.active_seconds != second.active_seconds) return nullptr;
        return &first;
    }
    return second.sequence > first.sequence ? &second : &first;
}

// Never roll persisted usage backwards if the trusted server clock regresses.
// A later trusted day starts a fresh bucket; the previous day is not carried.
struct WritePlan {
    std::uint8_t target_slot = 0;
    std::uint64_t sequence = 0;
    bool valid = false;
};

[[nodiscard]] inline bool MakeWritePlan(
    const Snapshot& first, const Snapshot& second, WritePlan* out) noexcept {
    if (out == nullptr) return false;
    const Snapshot* newest = SelectNewest(first, second);
    const std::uint64_t current = newest == nullptr ? 0 : newest->sequence;
    if (current == std::numeric_limits<std::uint64_t>::max()) return false;
    out->sequence = current + 1;
    out->target_slot = newest == nullptr ? 0 :
        static_cast<std::uint8_t>(newest == &first ? 1 : 0);
    out->valid = true;
    return true;
}

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
